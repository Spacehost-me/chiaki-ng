// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "discordpresence.h"
#include "settings.h"
#include "streamsession.h"

#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QLoggingCategory>
#include <QMetaObject>

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

namespace {

// Discord IPC opcodes
constexpr quint32 OP_HANDSHAKE = 0;
constexpr quint32 OP_FRAME = 1;
constexpr quint32 OP_CLOSE = 2;
constexpr quint32 OP_PING = 3;
constexpr quint32 OP_PONG = 4;

constexpr int IPC_MAX_PIPES = 10;
constexpr int IPC_CONNECT_TIMEOUT_MS = 1500;
constexpr int IPC_RECONNECT_MIN_MS = 5000;
constexpr int IPC_RECONNECT_MAX_MS = 60000;
// Discord rate limits presence updates to 5 per 20 seconds
constexpr int ACTIVITY_MIN_INTERVAL_MS = 4000;
constexpr int DISCORD_TEXT_MAX = 128;

constexpr int HOST_PROBE_PING_MS = 10000;
constexpr int HOST_PROBE_INITIAL_MS = 1500;
constexpr int HOST_PROBE_DROP_PINGS = 3;

constexpr int PROSPERO_MGR_DEFAULT_PORT = 7070;
constexpr int PROSPERO_MGR_TIMEOUT_MS = 5000;
// /api/titles re-runs the console's app.db query, so it is cached rather than polled.
// Short enough that the running game's play time still creeps up during a long session.
constexpr int PROSPERO_MGR_TITLES_TTL_S = 5 * 60;

constexpr int GAME_INFO_VALID_TTL_S = 30 * 24 * 60 * 60;
constexpr int GAME_INFO_INVALID_TTL_S = 60 * 60;
constexpr int GAME_INFO_INFLIGHT_TTL_S = 30;

// " · " (middle dot), spelled out to keep the source ASCII
const QString separator = QLatin1Char(' ') + QString(QChar(0x00B7)) + QLatin1Char(' ');
const char *CHIAKI_LOGO_URL = "https://raw.githubusercontent.com/streetpea/chiaki-ng/main/gui/chiaking.png";
const char *CHIAKI_WEBSITE_URL = "https://streetpea.github.io/chiaki-ng/";
// HMAC-SHA1 key for Sony's public title metadata database (PS4 titles)
const char *TMDB_HMAC_KEY_HEX = "F5DE66D2680E255B2DF79E74F890EBF349262F618BCAE2A9ACCDEE5156CE8DF2CDF2D48C71173CDC2594465B87405D197CF1AED3B7E9671EEB56CA6753C2E6B0";

QString clampText(QString text)
{
	text = text.simplified();
	if(text.size() > DISCORD_TEXT_MAX)
		text = text.left(DISCORD_TEXT_MAX - 1) + QChar(0x2026);
	// Discord rejects strings shorter than 2 characters
	if(text.size() == 1)
		text.append(QLatin1Char(' '));
	return text;
}

QString formatDuration(qint64 seconds)
{
	if(seconds < 60)
		return QObject::tr("%n minute(s)", "", 1);
	const qint64 hours = seconds / 3600;
	const qint64 minutes = (seconds % 3600) / 60;
	if(hours == 0)
		return QObject::tr("%n minute(s)", "", static_cast<int>(minutes));
	if(minutes == 0)
		return QObject::tr("%n hour(s)", "", static_cast<int>(hours));
	return QObject::tr("%1h %2m").arg(hours).arg(minutes);
}

bool isPs5TitleId(const QString &title_id)
{
	return title_id.startsWith(QLatin1String("PPSA"), Qt::CaseInsensitive);
}

bool isPs4TitleId(const QString &title_id)
{
	return title_id.startsWith(QLatin1String("CUSA"), Qt::CaseInsensitive);
}

QString normalizeTitleId(QString title_id)
{
	title_id = title_id.trimmed().toUpper();
	// Consoles may report the full NP title id (PPSA01474_00)
	const int underscore = title_id.indexOf(QLatin1Char('_'));
	if(underscore > 0)
		title_id.truncate(underscore);
	return title_id;
}

QStringList ipcSocketCandidates(int index)
{
	const QString name = QStringLiteral("discord-ipc-%1").arg(index);
	QStringList out;
#ifdef Q_OS_WIN
	// QLocalSocket maps a bare name to \\.\pipe\<name>
	out << name;
#else
	QStringList dirs;
	for(const char *env : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
	{
		const QString value = qEnvironmentVariable(env);
		if(!value.isEmpty() && !dirs.contains(value))
			dirs << value;
	}
	if(!dirs.contains(QStringLiteral("/tmp")))
		dirs << QStringLiteral("/tmp");
	// Sandboxed Discord installs put the socket in a sub directory
	const QStringList subdirs = {
		QString(),
		QStringLiteral("app/com.discordapp.Discord/"),
		QStringLiteral("app/com.discordapp.DiscordCanary/"),
		QStringLiteral("snap.discord/"),
		QStringLiteral("snap.discord-canary/"),
	};
	for(const QString &dir : dirs)
		for(const QString &sub : subdirs)
		{
			const QString path = dir + QLatin1Char('/') + sub + name;
			if(QFile::exists(path))
				out << path;
		}
#endif
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// ProsperoMgrClient
// ---------------------------------------------------------------------------

ProsperoMgrClient::ProsperoMgrClient(QObject *parent)
	: QObject(parent)
	, nam(new QNetworkAccessManager(this))
{
}

void ProsperoMgrClient::SetBaseUrl(const QString &url)
{
	QString normalized = url.trimmed();
	while(normalized.endsWith(QLatin1Char('/')))
		normalized.chop(1);
	if(!normalized.isEmpty() && !normalized.contains(QLatin1String("://")))
		normalized.prepend(QLatin1String("http://"));
	if(normalized == base_url)
		return;
	base_url = normalized;
	Reset();
}

void ProsperoMgrClient::Reset()
{
	poll_in_flight = false;
	reachable = false;
	last_error.clear();
	titles.clear();
	titles_fetched_at = QDateTime();
	titles_lookup_for.clear();
	const bool had_game = activity.HasGame();
	activity = DiscordConsoleActivity();
	if(had_game)
		emit ActivityUpdated();
}

void ProsperoMgrClient::Get(const QString &path, std::function<void(const QJsonDocument &)> on_ok, std::function<void(const QString &)> on_error)
{
	if(base_url.isEmpty())
	{
		if(on_error)
			on_error(tr("No Prospero Manager URL configured"));
		return;
	}
	QNetworkRequest request(QUrl(base_url + path));
	request.setRawHeader("Accept", "application/json");
	request.setTransferTimeout(PROSPERO_MGR_TIMEOUT_MS);
	QNetworkReply *reply = nam->get(request);
	const QString requested_base_url = base_url;
	connect(reply, &QNetworkReply::finished, this, [this, reply, on_ok, on_error, requested_base_url]() {
		reply->deleteLater();
		// Ignore late replies for a previous console / URL
		if(requested_base_url != base_url)
			return;
		if(reply->error() != QNetworkReply::NoError)
		{
			if(on_error)
				on_error(reply->errorString());
			return;
		}
		QJsonParseError parse_error;
		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parse_error);
		if(parse_error.error != QJsonParseError::NoError)
		{
			if(on_error)
				on_error(QObject::tr("Invalid JSON: %1").arg(parse_error.errorString()));
			return;
		}
		on_ok(doc);
	});
}

void ProsperoMgrClient::SetReachable(bool value, const QString &error)
{
	const bool changed = value != reachable || (!value && error != last_error);
	reachable = value;
	last_error = value ? QString() : error;
	if(changed)
		emit ReachabilityChanged(reachable, last_error);
}

void ProsperoMgrClient::Poll()
{
	if(base_url.isEmpty() || poll_in_flight)
		return;
	poll_in_flight = true;
	// Deliberately the cheap endpoint: Prospero Manager keeps it separate from
	// /api/titles so that "what is running" can be polled without a database query.
	Get(QStringLiteral("/api/titles/running"), [this](const QJsonDocument &doc) {
		poll_in_flight = false;
		SetReachable(true, QString());
		const QString title_id = normalizeTitleId(doc.object().value(QStringLiteral("title_id")).toString());
		if(title_id != activity.title_id)
		{
			activity.title_id = title_id;
			activity.title_name.clear();
			activity.content_version.clear();
			activity.total_play_seconds = -1;
			ApplyTitleMeta();
		}
		if(!title_id.isEmpty())
		{
			const bool stale = !titles_fetched_at.isValid()
				|| titles_fetched_at.secsTo(QDateTime::currentDateTimeUtc()) > PROSPERO_MGR_TITLES_TTL_S;
			// One lookup per newly seen title, then only on the TTL. An id the
			// console does not list (homebrew) must not re-run the database
			// query on every single poll.
			if(stale || (!titles.contains(title_id) && titles_lookup_for != title_id))
			{
				titles_lookup_for = title_id;
				RefreshTitles();
			}
		}
		if(want_stats)
		{
			RefreshThermal();
			RefreshStatus();
		}
		activity.updated_at = QDateTime::currentDateTimeUtc();
		emit ActivityUpdated();
	}, [this](const QString &error) {
		poll_in_flight = false;
		SetReachable(false, error);
		if(activity.HasGame())
		{
			activity = DiscordConsoleActivity();
			emit ActivityUpdated();
		}
	});
}

/** Copy the cached /api/titles row for the running game onto the activity. */
bool ProsperoMgrClient::ApplyTitleMeta()
{
	if(!activity.HasGame())
		return false;
	const auto it = titles.constFind(activity.title_id);
	if(it == titles.constEnd())
		return false;
	bool changed = false;
	if(!it->name.isEmpty() && it->name != activity.title_name)
	{
		activity.title_name = it->name;
		changed = true;
	}
	if(!it->version.isEmpty() && it->version != activity.content_version)
	{
		activity.content_version = it->version;
		changed = true;
	}
	if(it->played_seconds != activity.total_play_seconds)
	{
		activity.total_play_seconds = it->played_seconds;
		changed = true;
	}
	return changed;
}

// Name, version and play time all come from this one call, so it replaces the
// three separate lookups the old dashboard API needed.
void ProsperoMgrClient::RefreshTitles()
{
	// Stamp the attempt, not only the success, so a console that keeps failing
	// this call is not asked again on every poll.
	titles_fetched_at = QDateTime::currentDateTimeUtc();
	Get(QStringLiteral("/api/titles"), [this](const QJsonDocument &doc) {
		titles_fetched_at = QDateTime::currentDateTimeUtc();
		const QJsonArray rows = doc.object().value(QStringLiteral("titles")).toArray();
		titles.clear();
		for(const QJsonValue &value : rows)
		{
			const QJsonObject row = value.toObject();
			const QString id = normalizeTitleId(row.value(QStringLiteral("title_id")).toString());
			if(id.isEmpty())
				continue;
			TitleEntry entry;
			entry.name = row.value(QStringLiteral("name")).toString().trimmed();
			entry.version = row.value(QStringLiteral("version")).toString().trimmed();
			entry.played_seconds = static_cast<qint64>(row.value(QStringLiteral("played_seconds")).toDouble(-1));
			titles.insert(id, entry);
		}
		if(ApplyTitleMeta())
			emit ActivityUpdated();
	});
}

void ProsperoMgrClient::RefreshThermal()
{
	Get(QStringLiteral("/api/thermal"), [this](const QJsonDocument &doc) {
		const QJsonObject thermal = doc.object();
		const int cpu = thermal.value(QStringLiteral("cpu_c")).toInt(-1000);
		const int soc = thermal.value(QStringLiteral("soc_c")).toInt(-1000);
		const int fan = thermal.value(QStringLiteral("fan_duty_percent")).toInt(-1);
		if(cpu != activity.cpu_temp_c || soc != activity.soc_temp_c || fan != activity.fan_percent)
		{
			activity.cpu_temp_c = cpu;
			activity.soc_temp_c = soc;
			activity.fan_percent = fan;
			emit ActivityUpdated();
		}
	});
}

void ProsperoMgrClient::RefreshStatus()
{
	Get(QStringLiteral("/api/status"), [this](const QJsonDocument &doc) {
		// Prospero Manager's own process uptime, not the console's.
		const qint64 uptime = static_cast<qint64>(doc.object().value(QStringLiteral("uptime_seconds")).toDouble(-1));
		if(uptime != activity.uptime_seconds)
		{
			activity.uptime_seconds = uptime;
			emit ActivityUpdated();
		}
	});
}

// ---------------------------------------------------------------------------
// DiscordGameInfoResolver
// ---------------------------------------------------------------------------

DiscordGameInfoResolver::DiscordGameInfoResolver(QObject *parent)
	: QObject(parent)
	, nam(new QNetworkAccessManager(this))
{
	LoadCache();
}

QString DiscordGameInfoResolver::CachePath() const
{
	return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/discord-game-info.json");
}

void DiscordGameInfoResolver::LoadCache()
{
	QFile file(CachePath());
	if(!file.open(QIODevice::ReadOnly))
		return;
	const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	for(auto it = root.constBegin(); it != root.constEnd(); ++it)
	{
		const QJsonObject entry = it.value().toObject();
		DiscordGameInfo info;
		info.title_id = it.key();
		info.name = entry.value(QStringLiteral("name")).toString();
		info.icon_url = entry.value(QStringLiteral("icon")).toString();
		info.background_url = entry.value(QStringLiteral("background")).toString();
		info.fetched_at = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(entry.value(QStringLiteral("fetched_at")).toDouble()), Qt::UTC);
		info.valid = entry.value(QStringLiteral("valid")).toBool();
		if(!info.title_id.isEmpty())
			cache.insert(info.title_id, info);
	}
}

void DiscordGameInfoResolver::SaveCache()
{
	QJsonObject root;
	for(auto it = cache.constBegin(); it != cache.constEnd(); ++it)
	{
		const DiscordGameInfo &info = it.value();
		if(!info.valid)
			continue;
		QJsonObject entry;
		entry.insert(QStringLiteral("name"), info.name);
		entry.insert(QStringLiteral("icon"), info.icon_url);
		entry.insert(QStringLiteral("background"), info.background_url);
		entry.insert(QStringLiteral("fetched_at"), static_cast<double>(info.fetched_at.toSecsSinceEpoch()));
		entry.insert(QStringLiteral("valid"), info.valid);
		root.insert(it.key(), entry);
	}
	QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
	QFile file(CachePath());
	if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		qCWarning(chiakiGui) << "Discord: failed to write game info cache" << file.fileName();
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool DiscordGameInfoResolver::Lookup(const QString &title_id, DiscordGameInfo &out) const
{
	auto it = cache.constFind(normalizeTitleId(title_id));
	if(it == cache.constEnd())
		return false;
	const qint64 age = it->fetched_at.isValid() ? it->fetched_at.secsTo(QDateTime::currentDateTimeUtc()) : GAME_INFO_VALID_TTL_S + 1;
	if(age > (it->valid ? GAME_INFO_VALID_TTL_S : GAME_INFO_INVALID_TTL_S))
		return false;
	out = *it;
	return true;
}

void DiscordGameInfoResolver::Resolve(const QString &raw_title_id)
{
	const QString title_id = normalizeTitleId(raw_title_id);
	if(title_id.isEmpty())
		return;
	auto in_flight_it = in_flight.constFind(title_id);
	if(in_flight_it != in_flight.constEnd() && in_flight_it->secsTo(QDateTime::currentDateTimeUtc()) < GAME_INFO_INFLIGHT_TTL_S)
		return;
	in_flight.insert(title_id, QDateTime::currentDateTimeUtc());

	if(isPs5TitleId(title_id) || isPs4TitleId(title_id))
		ResolveFromPatchesDb(title_id);
	else
		Finish(title_id, DiscordGameInfo());
}

void DiscordGameInfoResolver::ResolveFromPatchesDb(const QString &title_id)
{
	const QString host = isPs5TitleId(title_id) ? QStringLiteral("prosperopatches.com") : QStringLiteral("orbispatches.com");
	QUrl url(QStringLiteral("https://%1/api/lookup").arg(host));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("titleid"), title_id);
	url.setQuery(query);
	QNetworkRequest request(url);
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("chiaki-ng/%1").arg(QStringLiteral(CHIAKI_VERSION)));
	request.setTransferTimeout(10000);
	QNetworkReply *reply = nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, title_id]() {
		reply->deleteLater();
		DiscordGameInfo info;
		info.title_id = title_id;
		if(reply->error() == QNetworkReply::NoError)
		{
			const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
			const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
			if(root.value(QStringLiteral("success")).toBool() && !metadata.isEmpty())
			{
				info.name = metadata.value(QStringLiteral("name")).toString().trimmed();
				info.icon_url = metadata.value(QStringLiteral("icon")).toString().trimmed();
				info.background_url = metadata.value(QStringLiteral("background")).toString().trimmed();
				info.valid = !info.name.isEmpty() || !info.icon_url.isEmpty();
			}
		}
		else
			qCInfo(chiakiGui) << "Discord: patches db lookup failed for" << title_id << reply->errorString();

		if(info.valid || !isPs4TitleId(title_id))
			Finish(title_id, info);
		else
			ResolveFromTmdb(title_id);
	});
}

void DiscordGameInfoResolver::ResolveFromTmdb(const QString &title_id)
{
	const QByteArray np_title_id = (title_id + QStringLiteral("_00")).toLatin1();
	const QByteArray key = QByteArray::fromHex(TMDB_HMAC_KEY_HEX);
	const QString digest = QString::fromLatin1(QMessageAuthenticationCode::hash(np_title_id, key, QCryptographicHash::Sha1).toHex().toUpper());
	const QUrl url(QStringLiteral("https://tmdb.np.dl.playstation.net/tmdb2/%1_%2/%1.json").arg(QString::fromLatin1(np_title_id), digest));
	QNetworkRequest request(url);
	request.setTransferTimeout(10000);
	QNetworkReply *reply = nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, title_id]() {
		reply->deleteLater();
		DiscordGameInfo info;
		info.title_id = title_id;
		if(reply->error() == QNetworkReply::NoError)
		{
			const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
			QString name;
			for(const QJsonValue &value : root.value(QStringLiteral("names")).toArray())
			{
				const QJsonObject entry = value.toObject();
				const QString candidate = entry.value(QStringLiteral("name")).toString().trimmed();
				if(candidate.isEmpty())
					continue;
				if(name.isEmpty() || entry.value(QStringLiteral("lang")).toString().startsWith(QLatin1String("en")))
					name = candidate;
			}
			QString icon;
			for(const QJsonValue &value : root.value(QStringLiteral("icons")).toArray())
			{
				icon = value.toObject().value(QStringLiteral("icon")).toString().trimmed();
				if(!icon.isEmpty())
					break;
			}
			QString background = root.value(QStringLiteral("backgroundImage")).toString().trimmed();
			// Discord's image proxy only fetches https URLs; Sony's CDN serves both
			for(QString *image : {&icon, &background})
				if(image->startsWith(QLatin1String("http://")))
					image->replace(0, 7, QLatin1String("https://"));
			info.name = name;
			info.icon_url = icon;
			info.background_url = background;
			info.valid = !info.name.isEmpty() || !info.icon_url.isEmpty();
		}
		else
			qCInfo(chiakiGui) << "Discord: TMDB lookup failed for" << title_id << reply->errorString();
		Finish(title_id, info);
	});
}

void DiscordGameInfoResolver::Finish(const QString &title_id, const DiscordGameInfo &resolved)
{
	DiscordGameInfo info = resolved;
	info.title_id = title_id;
	info.fetched_at = QDateTime::currentDateTimeUtc();
	cache.insert(title_id, info);
	in_flight.remove(title_id);
	if(info.valid)
		SaveCache();
	emit Resolved(title_id);
}

// ---------------------------------------------------------------------------
// DiscordPresence
// ---------------------------------------------------------------------------

DiscordPresence::DiscordPresence(Settings *settings, QObject *parent)
	: QObject(parent)
	, settings(settings)
	, prospero_mgr(new ProsperoMgrClient(this))
	, resolver(new DiscordGameInfoResolver(this))
{
	chiaki_log_init(&probe_log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING, chiaki_log_cb_print, nullptr);
	memset(&probe_service, 0, sizeof(probe_service));

	reconnect_timer.setSingleShot(true);
	connect(&reconnect_timer, &QTimer::timeout, this, &DiscordPresence::ConnectIpc);
	throttle_timer.setSingleShot(true);
	connect(&throttle_timer, &QTimer::timeout, this, &DiscordPresence::PushActivity);
	connect(&prospero_mgr_timer, &QTimer::timeout, prospero_mgr, &ProsperoMgrClient::Poll);
	connect(prospero_mgr, &ProsperoMgrClient::ActivityUpdated, this, &DiscordPresence::ProsperoMgrActivityUpdated);
	connect(prospero_mgr, &ProsperoMgrClient::ReachabilityChanged, this, &DiscordPresence::ProsperoMgrReachabilityChanged);
	connect(resolver, &DiscordGameInfoResolver::Resolved, this, &DiscordPresence::GameInfoResolved);

	SetStatus(tr("Disabled"));
	UpdateProsperoMgrStatus();
	SettingsChanged();
}

DiscordPresence::~DiscordPresence()
{
	StopHostProbe();
	if(socket)
	{
		if(ipc_ready)
		{
			// Best effort: clear the presence so it does not linger after exit
			QJsonObject args;
			args.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
			args.insert(QStringLiteral("activity"), QJsonValue::Null);
			QJsonObject payload;
			payload.insert(QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY"));
			payload.insert(QStringLiteral("args"), args);
			payload.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
			SendFrame(OP_FRAME, payload);
			socket->waitForBytesWritten(200);
		}
		socket->disconnect(this);
		socket->abort();
		delete socket;
		socket = nullptr;
	}
}

void DiscordPresence::SetSettings(Settings *new_settings)
{
	settings = new_settings;
	SettingsChanged();
}

void DiscordPresence::SetStatus(const QString &text)
{
	if(text == status_text)
		return;
	status_text = text;
	emit statusChanged();
}

// --- Discord IPC ------------------------------------------------------------

void DiscordPresence::ConnectIpc()
{
	if(!settings->GetDiscordPresenceEnabled())
		return;
	const QString client_id = settings->GetDiscordClientId();
	if(client_id.isEmpty())
	{
		SetStatus(tr("No Discord Application ID configured"));
		return;
	}
	if(socket)
	{
		if(socket->state() == QLocalSocket::ConnectedState || socket->state() == QLocalSocket::ConnectingState)
			return;
		socket->deleteLater();
		socket = nullptr;
	}
	QStringList candidates;
	for(int i = 0; i < IPC_MAX_PIPES; i++)
		candidates << ipcSocketCandidates(i);
	if(pipe_index >= candidates.size())
	{
		pipe_index = 0;
		reconnect_attempts++;
		const int delay = qMin(IPC_RECONNECT_MAX_MS, IPC_RECONNECT_MIN_MS * (1 << qMin(reconnect_attempts, 4)));
		SetStatus(tr("Discord not running, retrying..."));
		reconnect_timer.start(delay);
		return;
	}
	const QString target = candidates.at(pipe_index);
	active_client_id = client_id;
	socket = new QLocalSocket(this);
	connect(socket, &QLocalSocket::connected, this, &DiscordPresence::IpcConnected);
	connect(socket, &QLocalSocket::disconnected, this, &DiscordPresence::IpcDisconnected);
	connect(socket, &QLocalSocket::readyRead, this, &DiscordPresence::IpcReadyRead);
	connect(socket, &QLocalSocket::errorOccurred, this, &DiscordPresence::IpcError);
	SetStatus(tr("Connecting to Discord..."));
	QLocalSocket *s = socket;
	s->connectToServer(target);
	// A missing pipe fails synchronously (and IpcError has already moved on)
	if(socket != s)
		return;
	// Named pipes on Windows can hang around in a half-open state; bound the attempt
	QTimer::singleShot(IPC_CONNECT_TIMEOUT_MS, s, [this, s]() {
		if(socket != s || s->state() != QLocalSocket::ConnectingState)
			return;
		s->abort();
		// abort() does not necessarily report an error, so advance manually
		IpcError();
	});
}

void DiscordPresence::IpcConnected()
{
	rx_buffer.clear();
	QJsonObject handshake;
	handshake.insert(QStringLiteral("v"), 1);
	handshake.insert(QStringLiteral("client_id"), active_client_id);
	SendFrame(OP_HANDSHAKE, handshake);
}

void DiscordPresence::IpcError()
{
	if(!socket)
		return;
	if(socket->state() == QLocalSocket::ConnectedState)
		return;
	// Try the next candidate socket immediately; back off once all are exhausted
	const bool was_ready = ipc_ready;
	ipc_ready = false;
	socket->deleteLater();
	socket = nullptr;
	if(was_ready)
	{
		discord_user.clear();
		emit statusChanged();
		pipe_index = 0;
		reconnect_timer.start(IPC_RECONNECT_MIN_MS);
		SetStatus(tr("Disconnected from Discord, reconnecting..."));
		return;
	}
	pipe_index++;
	QTimer::singleShot(0, this, &DiscordPresence::ConnectIpc);
}

void DiscordPresence::IpcDisconnected()
{
	const bool was_ready = ipc_ready;
	CloseIpc(settings->GetDiscordPresenceEnabled());
	if(was_ready)
		SetStatus(tr("Disconnected from Discord, reconnecting..."));
}

void DiscordPresence::CloseIpc(bool schedule_reconnect)
{
	ipc_ready = false;
	activity_cleared = true;
	discord_user.clear();
	rx_buffer.clear();
	throttle_timer.stop();
	if(socket)
	{
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	emit statusChanged();
	if(schedule_reconnect)
	{
		pipe_index = 0;
		reconnect_timer.start(IPC_RECONNECT_MIN_MS);
	}
	else
		reconnect_timer.stop();
}

void DiscordPresence::SendFrame(quint32 opcode, const QJsonObject &payload)
{
	if(!socket || socket->state() != QLocalSocket::ConnectedState)
		return;
	const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	QByteArray frame;
	QDataStream stream(&frame, QIODevice::WriteOnly);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream << opcode << static_cast<quint32>(json.size());
	frame.append(json);
	socket->write(frame);
}

void DiscordPresence::IpcReadyRead()
{
	if(!socket)
		return;
	rx_buffer.append(socket->readAll());
	while(rx_buffer.size() >= 8)
	{
		QDataStream stream(rx_buffer);
		stream.setByteOrder(QDataStream::LittleEndian);
		quint32 opcode = 0, length = 0;
		stream >> opcode >> length;
		if(length > 16 * 1024 * 1024)
		{
			qCWarning(chiakiGui) << "Discord: dropping oversized IPC frame";
			CloseIpc(true);
			return;
		}
		if(rx_buffer.size() < static_cast<int>(8 + length))
			break;
		const QByteArray json = rx_buffer.mid(8, static_cast<int>(length));
		rx_buffer.remove(0, static_cast<int>(8 + length));
		HandleFrame(opcode, QJsonDocument::fromJson(json).object());
		if(!socket)
			return;
	}
}

void DiscordPresence::HandleFrame(quint32 opcode, const QJsonObject &payload)
{
	switch(opcode)
	{
		case OP_PING:
			SendFrame(OP_PONG, payload);
			return;
		case OP_CLOSE:
		{
			const int code = payload.value(QStringLiteral("code")).toInt();
			const QString message = payload.value(QStringLiteral("message")).toString();
			qCWarning(chiakiGui) << "Discord: IPC closed" << code << message;
			CloseIpc(code != 4000);
			if(code == 4000)
				SetStatus(tr("Discord rejected the Application ID: %1").arg(message));
			else
				SetStatus(tr("Disconnected from Discord (%1), reconnecting...").arg(message));
			return;
		}
		case OP_FRAME:
			break;
		default:
			return;
	}

	const QString evt = payload.value(QStringLiteral("evt")).toString();
	const QString cmd = payload.value(QStringLiteral("cmd")).toString();
	const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
	if(evt == QLatin1String("READY"))
	{
		const QJsonObject user = data.value(QStringLiteral("user")).toObject();
		discord_user = user.value(QStringLiteral("global_name")).toString();
		if(discord_user.isEmpty())
			discord_user = user.value(QStringLiteral("username")).toString();
		ipc_ready = true;
		reconnect_attempts = 0;
		pipe_index = 0;
		activity_cleared = true;
		emit statusChanged();
		SetStatus(discord_user.isEmpty() ? tr("Connected to Discord") : tr("Connected to Discord as %1").arg(discord_user));
		activity_dirty = true;
		PushActivity();
	}
	else if(evt == QLatin1String("ERROR"))
	{
		const int code = data.value(QStringLiteral("code")).toInt();
		const QString message = data.value(QStringLiteral("message")).toString();
		qCWarning(chiakiGui) << "Discord: IPC error" << code << message << "for" << cmd;
		if(!ipc_ready)
		{
			CloseIpc(code != 4000 && code != 1000);
			SetStatus(tr("Discord error: %1").arg(message));
		}
		else
			SetStatus(tr("Connected to Discord (last error: %1)").arg(message));
	}
}

void DiscordPresence::reconnect()
{
	reconnect_attempts = 0;
	pipe_index = 0;
	CloseIpc(false);
	if(settings->GetDiscordPresenceEnabled())
		ConnectIpc();
	else
		SetStatus(tr("Disabled"));
}

// --- Activity -----------------------------------------------------------------

void DiscordPresence::ScheduleActivityUpdate()
{
	preview = BuildActivity();
	emit previewChanged();

	activity_dirty = true;
	if(!ipc_ready)
		return;
	if(!last_push.isValid() || last_push.elapsed() >= ACTIVITY_MIN_INTERVAL_MS)
		PushActivity();
	else if(!throttle_timer.isActive())
		throttle_timer.start(static_cast<int>(ACTIVITY_MIN_INTERVAL_MS - last_push.elapsed()));
}

void DiscordPresence::PushActivity()
{
	if(!ipc_ready || !activity_dirty)
		return;
	activity_dirty = false;
	const QJsonObject activity = BuildActivity();
	if(activity.isEmpty() && activity_cleared)
		return;
	QJsonObject args;
	args.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
	if(activity.isEmpty())
		args.insert(QStringLiteral("activity"), QJsonValue::Null);
	else
		args.insert(QStringLiteral("activity"), activity);
	QJsonObject payload;
	payload.insert(QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY"));
	payload.insert(QStringLiteral("args"), args);
	payload.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
	SendFrame(OP_FRAME, payload);
	activity_cleared = activity.isEmpty();
	last_push.restart();
}

QString DiscordPresence::CurrentTitleId() const
{
	if(phase == Phase::Idle)
		return QString();
	if(!discovery_title_id.isEmpty())
		return discovery_title_id;
	if(ProsperoMgrEnabled())
		return prospero_mgr->GetActivity().title_id;
	return QString();
}

QString DiscordPresence::CurrentGameName() const
{
	const QString title_id = CurrentTitleId();
	if(title_id.isEmpty())
		return QString();
	DiscordGameInfo info;
	if(resolver->Lookup(title_id, info) && info.valid && !info.name.isEmpty())
		return info.name;
	if(!discovery_app_name.isEmpty() && discovery_title_id == title_id)
		return discovery_app_name;
	const DiscordConsoleActivity &console = prospero_mgr->GetActivity();
	if(console.title_id == title_id && !console.title_name.isEmpty())
		return console.title_name;
	return QString();
}

QString DiscordPresence::ConsoleLabel() const
{
	const QString generic = session_ps5 ? QStringLiteral("PS5") : QStringLiteral("PS4");
	if(!settings->GetDiscordShowConsoleName())
		return generic;
	if(!discovery_host_name.isEmpty())
		return discovery_host_name;
	if(!session_nickname.isEmpty())
		return session_nickname;
	return generic;
}

QString DiscordPresence::StreamLabel() const
{
	QStringList parts;
	parts << ConsoleLabel();
	if(settings->GetDiscordShowStreamInfo() && session_height > 0)
	{
		QString quality = QStringLiteral("%1p").arg(session_height);
		if(session_fps > 0)
			quality += QString::number(session_fps);
		parts << quality;
		if(!session_codec.isEmpty())
			parts << session_codec;
	}
	if(session_remote)
		parts << tr("via PSN");
	if(settings->GetDiscordShowConsoleStats() && ProsperoMgrEnabled())
	{
		const DiscordConsoleActivity &console = prospero_mgr->GetActivity();
		if(console.cpu_temp_c > -1000)
			parts << tr("CPU %1%2C").arg(console.cpu_temp_c).arg(QChar(0x00B0));
	}
	return parts.join(separator);
}

QJsonObject DiscordPresence::BuildActivity() const
{
	if(!settings->GetDiscordPresenceEnabled())
		return QJsonObject();
	if(phase == Phase::Idle && !settings->GetDiscordShowIdle())
		return QJsonObject();

	const QString title_id = CurrentTitleId();
	DiscordGameInfo info;
	const bool have_info = !title_id.isEmpty() && resolver->Lookup(title_id, info) && info.valid;
	const QString game = CurrentGameName();
	const bool show_game = settings->GetDiscordShowGame() && !game.isEmpty();
	const DiscordConsoleActivity &console = prospero_mgr->GetActivity();
	const bool console_info = ProsperoMgrEnabled() && !title_id.isEmpty() && console.title_id == title_id;

	QJsonObject activity;
	activity.insert(QStringLiteral("type"), 0); // Playing
	QJsonObject assets;
	QString details, state, large_text;
	bool cover_art = false;

	switch(phase)
	{
		case Phase::Idle:
		{
			details = tr("In the launcher");
			if(idle_host_count > 0)
				state = tr("%n console(s) available", "", idle_host_count);
			else
				state = tr("Looking for consoles");
			// A registered console is already running something: mention it
			if(!idle_title_id.isEmpty() && settings->GetDiscordShowGame())
			{
				DiscordGameInfo idle_info;
				QString idle_game = idle_app_name;
				if(resolver->Lookup(idle_title_id, idle_info) && idle_info.valid && !idle_info.name.isEmpty())
					idle_game = idle_info.name;
				if(!idle_game.isEmpty())
				{
					const QString console_name = (settings->GetDiscordShowConsoleName() && !idle_host_name.isEmpty())
						? idle_host_name : (idle_ps5 ? QStringLiteral("PS5") : QStringLiteral("PS4"));
					state = tr("%1 is running %2").arg(console_name, idle_game);
				}
			}
			large_text = tr("chiaki-ng");
			break;
		}
		case Phase::Connecting:
			details = tr("Connecting to %1...").arg(ConsoleLabel());
			state = session_remote ? tr("Remote Play via PSN") : tr("Remote Play");
			large_text = tr("chiaki-ng");
			break;
		case Phase::Streaming:
			details = show_game ? game : tr("Remote Play");
			state = StreamLabel();
			if(show_game)
			{
				QStringList hover;
				hover << game;
				if(console_info && !console.content_version.isEmpty())
					hover << tr("v%1").arg(console.content_version);
				if(settings->GetDiscordShowPlaytime() && console_info && console.total_play_seconds > 0)
					hover << tr("%1 played").arg(formatDuration(console.total_play_seconds));
				large_text = hover.join(separator);
				if(settings->GetDiscordGameAsActivityName())
					activity.insert(QStringLiteral("name"), clampText(game));
				else
					activity.insert(QStringLiteral("status_display_type"), 2); // show details (the game) as the status
				cover_art = settings->GetDiscordShowCoverArt() && have_info && !info.icon_url.isEmpty();
			}
			else if(!title_id.isEmpty())
				large_text = title_id;
			else
				large_text = tr("On the home screen");
			break;
	}

	activity.insert(QStringLiteral("details"), clampText(details));
	if(!state.isEmpty())
		activity.insert(QStringLiteral("state"), clampText(state));

	if(cover_art)
	{
		assets.insert(QStringLiteral("large_image"), info.icon_url);
		assets.insert(QStringLiteral("large_text"), clampText(large_text));
		assets.insert(QStringLiteral("small_image"), QString::fromLatin1(CHIAKI_LOGO_URL));
		QString small_text = tr("chiaki-ng %1").arg(QStringLiteral(CHIAKI_VERSION));
		if(settings->GetDiscordShowConsoleStats() && console_info && console.soc_temp_c > -1000)
			small_text += separator + tr("SoC %1%2C").arg(console.soc_temp_c).arg(QChar(0x00B0));
		if(settings->GetDiscordShowConsoleStats() && console_info && console.fan_percent >= 0)
			small_text += separator + tr("Fan %1%").arg(console.fan_percent);
		if(settings->GetDiscordShowConsoleStats() && console_info && console.uptime_seconds > 0)
			small_text += separator + tr("Prospero Manager up %1").arg(formatDuration(console.uptime_seconds));
		assets.insert(QStringLiteral("small_text"), clampText(small_text));
	}
	else
	{
		assets.insert(QStringLiteral("large_image"), QString::fromLatin1(CHIAKI_LOGO_URL));
		assets.insert(QStringLiteral("large_text"), clampText(large_text.isEmpty() ? tr("chiaki-ng %1").arg(QStringLiteral(CHIAKI_VERSION)) : large_text));
	}
	activity.insert(QStringLiteral("assets"), assets);

	if(settings->GetDiscordShowElapsedTime() && phase != Phase::Idle)
	{
		const QDateTime start = connected_at.isValid() ? connected_at : session_started_at;
		if(start.isValid())
		{
			QJsonObject timestamps;
			timestamps.insert(QStringLiteral("start"), static_cast<double>(start.toSecsSinceEpoch()));
			activity.insert(QStringLiteral("timestamps"), timestamps);
		}
	}

	if(settings->GetDiscordShowButtons())
	{
		QJsonArray buttons;
		QJsonObject button;
		button.insert(QStringLiteral("label"), tr("Get chiaki-ng"));
		button.insert(QStringLiteral("url"), QString::fromLatin1(CHIAKI_WEBSITE_URL));
		buttons.append(button);
		activity.insert(QStringLiteral("buttons"), buttons);
	}

	return activity;
}

// --- Session lifecycle ----------------------------------------------------------

void DiscordPresence::SessionCreated(const StreamSessionConnectInfo &info)
{
	StopHostProbe();
	phase = Phase::Connecting;
	session_host = info.host;
	session_nickname = info.nickname;
	session_ps5 = chiaki_target_is_ps5(info.target);
	session_remote = !info.duid.isEmpty();
	session_width = info.video_profile.width;
	session_height = info.video_profile.height;
	session_fps = info.video_profile.max_fps;
	switch(info.video_profile.codec)
	{
		case CHIAKI_CODEC_H264:
			session_codec = QStringLiteral("H.264");
			break;
		case CHIAKI_CODEC_H265:
			session_codec = QStringLiteral("HEVC");
			break;
		case CHIAKI_CODEC_H265_HDR:
			session_codec = QStringLiteral("HEVC HDR");
			break;
		default:
			session_codec.clear();
			break;
	}
	session_started_at = QDateTime::currentDateTimeUtc();
	connected_at = QDateTime();
	discovery_title_id.clear();
	discovery_app_name.clear();
	discovery_host_name.clear();
	prospero_mgr->Reset();
	ScheduleActivityUpdate();
}

void DiscordPresence::SessionConnected()
{
	if(phase == Phase::Idle)
		return;
	phase = Phase::Streaming;
	connected_at = QDateTime::currentDateTimeUtc();
	StartHostProbe();
	StartProsperoMgrPolling();
	ScheduleActivityUpdate();
}

void DiscordPresence::SessionEnded()
{
	StopHostProbe();
	StopProsperoMgrPolling();
	prospero_mgr_testing = false;
	phase = Phase::Idle;
	session_host.clear();
	session_nickname.clear();
	session_codec.clear();
	session_started_at = QDateTime();
	connected_at = QDateTime();
	discovery_title_id.clear();
	discovery_app_name.clear();
	discovery_host_name.clear();
	prospero_mgr->Reset();
	UpdateProsperoMgrStatus();
	ScheduleActivityUpdate();
}

void DiscordPresence::SetIdleHosts(int count, const QString &host_name, bool ps5, const QString &app_name, const QString &title_id)
{
	const QString normalized = normalizeTitleId(title_id);
	if(count == idle_host_count && host_name == idle_host_name && ps5 == idle_ps5 && app_name == idle_app_name && normalized == idle_title_id)
		return;
	idle_host_count = count;
	idle_host_name = host_name;
	idle_ps5 = ps5;
	idle_app_name = app_name;
	idle_title_id = normalized;
	// Warm the cover art cache so it is ready the moment a stream starts
	if(!idle_title_id.isEmpty() && settings->GetDiscordPresenceEnabled())
	{
		DiscordGameInfo info;
		if(!resolver->Lookup(idle_title_id, info))
			resolver->Resolve(idle_title_id);
	}
	if(phase == Phase::Idle)
		ScheduleActivityUpdate();
}

void DiscordPresence::SettingsChanged()
{
	const bool enabled = settings->GetDiscordPresenceEnabled();
	const QString client_id = settings->GetDiscordClientId();

	prospero_mgr->SetWantStats(settings->GetDiscordShowConsoleStats());
	if(phase == Phase::Streaming)
	{
		if(ProsperoMgrEnabled())
			StartProsperoMgrPolling();
		else
			StopProsperoMgrPolling();
	}
	UpdateProsperoMgrStatus();

	if(!enabled)
	{
		if(ipc_ready)
		{
			activity_dirty = true;
			PushActivity();
			if(socket)
				socket->waitForBytesWritten(200);
		}
		CloseIpc(false);
		SetStatus(tr("Disabled"));
		preview = QJsonObject();
		emit previewChanged();
		return;
	}
	if(client_id.isEmpty())
	{
		CloseIpc(false);
		SetStatus(tr("No Discord Application ID configured"));
		preview = BuildActivity();
		emit previewChanged();
		return;
	}
	if(client_id != active_client_id || (!socket && !reconnect_timer.isActive()))
	{
		reconnect_attempts = 0;
		pipe_index = 0;
		CloseIpc(false);
		ConnectIpc();
	}
	ScheduleActivityUpdate();
}

void DiscordPresence::GameInfoResolved(const QString &title_id)
{
	if(title_id == CurrentTitleId() || (phase == Phase::Idle && title_id == idle_title_id))
		ScheduleActivityUpdate();
}

// --- Running game via chiaki discovery ----------------------------------------------

void DiscordPresence::HostProbeCallback(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
	DiscordPresence *self = reinterpret_cast<DiscordPresence *>(user);
	QString title_id, app_name, host_name;
	if(hosts_count > 0)
	{
		const ChiakiDiscoveryHost &host = hosts[0];
		if(host.running_app_titleid)
			title_id = QString::fromLocal8Bit(host.running_app_titleid);
		if(host.running_app_name)
			app_name = QString::fromLocal8Bit(host.running_app_name);
		if(host.host_name)
			host_name = QString::fromLocal8Bit(host.host_name);
	}
	// Called on the discovery thread
	QMetaObject::invokeMethod(self, "DiscoveryHostUpdated", Qt::QueuedConnection,
		Q_ARG(QString, title_id), Q_ARG(QString, app_name), Q_ARG(QString, host_name));
}

void DiscordPresence::DiscoveryHostUpdated(const QString &title_id, const QString &app_name, const QString &host_name)
{
	if(!probe_active)
		return;
	const QString normalized = normalizeTitleId(title_id);
	const bool changed = normalized != discovery_title_id || app_name != discovery_app_name || host_name != discovery_host_name;
	discovery_title_id = normalized;
	discovery_app_name = app_name.trimmed();
	discovery_host_name = host_name.trimmed();
	if(!discovery_title_id.isEmpty())
	{
		DiscordGameInfo info;
		if(!resolver->Lookup(discovery_title_id, info))
			resolver->Resolve(discovery_title_id);
	}
	if(changed)
		ScheduleActivityUpdate();
}

void DiscordPresence::StartHostProbe()
{
	if(probe_active || session_remote || session_host.isEmpty())
		return;
	if(!settings->GetDiscordPresenceEnabled())
		return;

	ChiakiDiscoveryServiceOptions options = {};
	options.ping_ms = HOST_PROBE_PING_MS;
	options.ping_initial_ms = HOST_PROBE_INITIAL_MS;
	options.hosts_max = 1;
	options.host_drop_pings = HOST_PROBE_DROP_PINGS;
	options.cb = HostProbeCallback;
	options.cb_user = this;

	QByteArray host_utf8 = session_host.toUtf8();
	options.send_host = host_utf8.data();
	struct sockaddr_storage addr = {};
	if(session_host.contains(QLatin1Char(':')))
	{
		addr.ss_family = AF_INET6;
		options.send_addr = &addr;
		options.send_addr_size = sizeof(struct sockaddr_in6);
	}
	else
	{
		addr.ss_family = AF_INET;
		options.send_addr = &addr;
		options.send_addr_size = sizeof(struct sockaddr_in);
	}
	ChiakiErrorCode err = chiaki_discovery_service_init(&probe_service, &options, &probe_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		qCWarning(chiakiGui) << "Discord: failed to start running-app probe for" << session_host << chiaki_error_string(err);
		return;
	}
	probe_active = true;
}

void DiscordPresence::StopHostProbe()
{
	if(!probe_active)
		return;
	probe_active = false;
	chiaki_discovery_service_fini(&probe_service);
	memset(&probe_service, 0, sizeof(probe_service));
}

// --- Prospero Manager -----------------------------------------------------------------

bool DiscordPresence::ProsperoMgrEnabled() const
{
	return settings->GetDiscordProsperoMgrEnabled();
}

QString DiscordPresence::ProsperoMgrUrl() const
{
	QString url = settings->GetDiscordProsperoMgrUrl();
	if(!url.isEmpty())
		return url;
	if(session_host.isEmpty() || session_remote)
		return QString();
	// Default to the dashboard port on the console we are streaming from
	if(session_host.contains(QLatin1Char(':')))
		return QStringLiteral("http://[%1]:%2").arg(session_host).arg(PROSPERO_MGR_DEFAULT_PORT);
	return QStringLiteral("http://%1:%2").arg(session_host).arg(PROSPERO_MGR_DEFAULT_PORT);
}

void DiscordPresence::StartProsperoMgrPolling()
{
	prospero_mgr_testing = false;
	if(!ProsperoMgrEnabled() || !settings->GetDiscordPresenceEnabled())
	{
		StopProsperoMgrPolling();
		return;
	}
	const QString url = ProsperoMgrUrl();
	prospero_mgr->SetBaseUrl(url);
	if(url.isEmpty())
	{
		StopProsperoMgrPolling();
		UpdateProsperoMgrStatus();
		return;
	}
	const int interval_ms = static_cast<int>(settings->GetDiscordProsperoMgrPollInterval()) * 1000;
	if(!prospero_mgr_timer.isActive() || prospero_mgr_timer.interval() != interval_ms)
	{
		prospero_mgr_timer.start(interval_ms);
		prospero_mgr->Poll();
	}
}

void DiscordPresence::StopProsperoMgrPolling()
{
	prospero_mgr_timer.stop();
}

void DiscordPresence::ProsperoMgrActivityUpdated()
{
	const DiscordConsoleActivity &console = prospero_mgr->GetActivity();
	if(console.HasGame())
	{
		DiscordGameInfo info;
		if(!resolver->Lookup(console.title_id, info))
			resolver->Resolve(console.title_id);
	}
	UpdateProsperoMgrStatus();
	if(phase != Phase::Idle)
		ScheduleActivityUpdate();
}

void DiscordPresence::ProsperoMgrReachabilityChanged(bool, const QString &)
{
	UpdateProsperoMgrStatus();
}

void DiscordPresence::UpdateProsperoMgrStatus()
{
	QString text;
	const QString configured = settings->GetDiscordProsperoMgrUrl();
	const QString effective = configured.isEmpty() ? tr("http://<console address>:%1").arg(PROSPERO_MGR_DEFAULT_PORT) : configured;
	if(!ProsperoMgrEnabled())
		text = tr("Disabled");
	else if(!prospero_mgr_timer.isActive() && !prospero_mgr_testing)
		text = tr("Idle - %1 is polled while streaming").arg(effective);
	else if(prospero_mgr->GetBaseUrl().isEmpty())
		text = tr("No console address available for this session, enter the URL manually");
	else if(prospero_mgr->IsReachable())
	{
		const DiscordConsoleActivity &console = prospero_mgr->GetActivity();
		if(console.HasGame())
			text = tr("Connected to %1 - running %2").arg(prospero_mgr->GetBaseUrl(), console.title_name.isEmpty() ? console.title_id : console.title_name);
		else
			text = tr("Connected to %1 - no game running").arg(prospero_mgr->GetBaseUrl());
	}
	else if(!prospero_mgr->GetLastError().isEmpty())
		text = tr("Cannot reach %1: %2").arg(prospero_mgr->GetBaseUrl(), prospero_mgr->GetLastError());
	else
		text = tr("Waiting for %1...").arg(prospero_mgr->GetBaseUrl());
	if(text == prospero_mgr_status)
		return;
	prospero_mgr_status = text;
	emit prosperoMgrStatusChanged();
}

void DiscordPresence::testProsperoMgr()
{
	QString url = settings->GetDiscordProsperoMgrUrl();
	if(url.isEmpty())
		url = ProsperoMgrUrl();
	if(url.isEmpty())
	{
		prospero_mgr_status = tr("Enter the Prospero Manager URL (or start a stream to use the console address)");
		emit prosperoMgrStatusChanged();
		return;
	}
	prospero_mgr_testing = true;
	prospero_mgr->SetBaseUrl(url);
	prospero_mgr_status = tr("Testing %1...").arg(url);
	emit prosperoMgrStatusChanged();
	prospero_mgr->Poll();
}
