// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_DISCORDPRESENCE_H
#define CHIAKI_DISCORDPRESENCE_H

#include <chiaki/discoveryservice.h>
#include <chiaki/log.h>

#include <QObject>
#include <QString>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QElapsedTimer>

#include <functional>

class Settings;
struct StreamSessionConnectInfo;
class QLocalSocket;
class QNetworkAccessManager;

/**
 * Publicly resolvable metadata for a PlayStation title id (e.g. PPSA01474 / CUSA01715).
 * Cover art has to be an https URL reachable from Discord's image proxy, so this is
 * resolved from public title databases rather than from the console itself.
 */
struct DiscordGameInfo
{
	QString title_id;
	QString name;
	QString icon_url;
	QString background_url;
	QDateTime fetched_at;
	bool valid = false;
};

/**
 * Snapshot of the running game as reported by the Prospero Manager web UI
 * (ProsperoMgr) running on a jailbroken console.
 */
struct DiscordConsoleActivity
{
	QString title_id;
	QString title_name;
	QString content_version;
	qint64 total_play_seconds = -1;
	int cpu_temp_c = -1000;
	int soc_temp_c = -1000;
	int fan_percent = -1;
	qint64 uptime_seconds = -1;
	QDateTime updated_at;

	bool HasGame() const { return !title_id.isEmpty(); }
};

/**
 * Talks to the Prospero Manager web API on the console and exposes the currently
 * running title, play time and thermals.
 */
class ProsperoMgrClient : public QObject
{
	Q_OBJECT

	public:
		explicit ProsperoMgrClient(QObject *parent = nullptr);

		void SetBaseUrl(const QString &url);
		QString GetBaseUrl() const { return base_url; }
		void SetWantStats(bool want) { want_stats = want; }

		void Poll();
		void Reset();

		const DiscordConsoleActivity &GetActivity() const { return activity; }
		QString GetLastError() const { return last_error; }
		bool IsReachable() const { return reachable; }
		int GetLibrarySize() const { return titles.size(); }

	signals:
		void ActivityUpdated();
		void ReachabilityChanged(bool reachable, const QString &error);

	private:
		/** One row of /api/titles: everything Prospero Manager knows about an installed title. */
		struct TitleEntry
		{
			QString name;
			QString version;
			qint64 played_seconds = -1;
		};

		void Get(const QString &path, std::function<void(const QJsonDocument &)> on_ok, std::function<void(const QString &)> on_error = {});
		void SetReachable(bool value, const QString &error);
		void RefreshTitles();
		bool ApplyTitleMeta();
		void RefreshThermal();
		void RefreshStatus();

		QNetworkAccessManager *nam;
		QString base_url;
		bool want_stats = false;
		bool reachable = false;
		bool poll_in_flight = false;
		QString last_error;
		QHash<QString, TitleEntry> titles;
		QDateTime titles_fetched_at;
		QString titles_lookup_for;
		DiscordConsoleActivity activity;
};

/**
 * Resolves title ids to public names and cover art (prosperopatches.com for PS5,
 * orbispatches.com then Sony's TMDB for PS4) and caches the result on disk.
 */
class DiscordGameInfoResolver : public QObject
{
	Q_OBJECT

	public:
		explicit DiscordGameInfoResolver(QObject *parent = nullptr);

		bool Lookup(const QString &title_id, DiscordGameInfo &out) const;
		void Resolve(const QString &title_id);

		/**
		 * Last resort for titles no public database knows (unreleased, homebrew,
		 * regional): grab the icon off the console and mirror it to Imgur, since
		 * Discord's image proxy cannot reach a console on the local network.
		 * Opt-in; without it such titles keep the chiaki-ng logo.
		 */
		void SetCoverUpload(bool enabled, const QString &client_id);
		void SetConsoleIconSource(const QString &base_url);

	signals:
		void Resolved(const QString &title_id);

	private:
		void ResolveFromPatchesDb(const QString &title_id);
		void ResolveFromTmdb(const QString &title_id);
		void ResolveFromConsole(const QString &title_id);
		void UploadCover(const QString &title_id, const QByteArray &image);
		void Finish(const QString &title_id, const DiscordGameInfo &info);
		void LoadCache();
		void SaveCache();
		QString CachePath() const;

		QNetworkAccessManager *nam;
		QHash<QString, DiscordGameInfo> cache;
		QHash<QString, QDateTime> in_flight;
		QString console_icon_base;
		QString imgur_client_id;
		bool upload_enabled = false;
};

class DiscordPresence : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QString status READ status NOTIFY statusChanged)
	Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
	Q_PROPERTY(QString discordUser READ discordUser NOTIFY statusChanged)
	Q_PROPERTY(QString prosperoMgrStatus READ prosperoMgrStatus NOTIFY prosperoMgrStatusChanged)
	Q_PROPERTY(QString previewName READ previewName NOTIFY previewChanged)
	Q_PROPERTY(QString previewDetails READ previewDetails NOTIFY previewChanged)
	Q_PROPERTY(QString previewState READ previewState NOTIFY previewChanged)
	Q_PROPERTY(QString previewLargeText READ previewLargeText NOTIFY previewChanged)
	Q_PROPERTY(QString previewLargeImage READ previewLargeImage NOTIFY previewChanged)
	Q_PROPERTY(QString previewSmallImage READ previewSmallImage NOTIFY previewChanged)
	Q_PROPERTY(QString previewSmallText READ previewSmallText NOTIFY previewChanged)

	public:
		enum class Phase
		{
			Idle,
			Connecting,
			Streaming
		};

		DiscordPresence(Settings *settings, QObject *parent = nullptr);
		~DiscordPresence();

		QString status() const { return status_text; }
		bool connected() const { return ipc_ready; }
		QString discordUser() const { return discord_user; }
		QString prosperoMgrStatus() const { return prospero_mgr_status; }
		QString previewName() const { return preview.value("name").toString(); }
		QString previewDetails() const { return preview.value("details").toString(); }
		QString previewState() const { return preview.value("state").toString(); }
		QString previewLargeText() const { return preview.value("assets").toObject().value("large_text").toString(); }
		QString previewLargeImage() const { return preview.value("assets").toObject().value("large_image").toString(); }
		QString previewSmallImage() const { return preview.value("assets").toObject().value("small_image").toString(); }
		QString previewSmallText() const { return preview.value("assets").toObject().value("small_text").toString(); }

		// Session lifecycle, driven by QmlBackend
		void SessionCreated(const StreamSessionConnectInfo &info);
		void SessionConnected();
		void SessionEnded();

		// Launcher state, driven by QmlBackend's discovery updates
		void SetIdleHosts(int count, const QString &host_name, bool ps5, const QString &app_name, const QString &title_id);

		void SetSettings(Settings *new_settings);

		Q_INVOKABLE void reconnect();
		Q_INVOKABLE void testProsperoMgr();

	public slots:
		void SettingsChanged();

	signals:
		void statusChanged();
		void prosperoMgrStatusChanged();
		void previewChanged();

	private slots:
		void IpcConnected();
		void IpcDisconnected();
		void IpcReadyRead();
		void IpcError();
		void DiscoveryHostUpdated(const QString &title_id, const QString &app_name, const QString &host_name);
		void ProsperoMgrActivityUpdated();
		void ProsperoMgrReachabilityChanged(bool reachable, const QString &error);
		void GameInfoResolved(const QString &title_id);

	private:
		void ConnectIpc();
		void CloseIpc(bool schedule_reconnect);
		void SendFrame(quint32 opcode, const QJsonObject &payload);
		void HandleFrame(quint32 opcode, const QJsonObject &payload);
		void SetStatus(const QString &text);

		void ScheduleActivityUpdate();
		void PushActivity();
		QJsonObject BuildActivity() const;
		QString CurrentTitleId() const;
		QString CurrentGameName() const;
		QString ConsoleLabel() const;
		QString StreamLabel() const;
		QString ProsperoMgrUrl() const;
		bool ProsperoMgrEnabled() const;
		void UpdateProsperoMgrStatus();
		void UpdateCoverSources();

		void StartHostProbe();
		void StopHostProbe();
		void StartProsperoMgrPolling();
		void StopProsperoMgrPolling();

		static void HostProbeCallback(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user);

		Settings *settings;

		// Discord IPC
		QLocalSocket *socket = nullptr;
		QByteArray rx_buffer;
		QTimer reconnect_timer;
		QTimer throttle_timer;
		int reconnect_attempts = 0;
		int pipe_index = 0;
		bool ipc_ready = false;
		bool activity_dirty = false;
		bool activity_cleared = true;
		QElapsedTimer last_push;
		QString status_text;
		QString discord_user;
		QString active_client_id;
		QJsonObject preview;

		// Stream state
		Phase phase = Phase::Idle;
		QString session_host;
		QString session_nickname;
		bool session_ps5 = true;
		bool session_remote = false;
		unsigned int session_width = 0;
		unsigned int session_height = 0;
		unsigned int session_fps = 0;
		QString session_codec;
		QDateTime session_started_at;
		QDateTime connected_at;

		// Running game, from chiaki discovery (unicast probe of the streaming host)
		ChiakiLog probe_log;
		ChiakiDiscoveryService probe_service;
		bool probe_active = false;
		QString discovery_title_id;
		QString discovery_app_name;
		QString discovery_host_name;

		// Launcher (idle) info
		int idle_host_count = 0;
		QString idle_host_name;
		QString idle_app_name;
		QString idle_title_id;
		bool idle_ps5 = true;

		// Prospero Manager
		ProsperoMgrClient *prospero_mgr;
		QTimer prospero_mgr_timer;
		QString prospero_mgr_status;
		bool prospero_mgr_testing = false;

		DiscordGameInfoResolver *resolver;
};

#endif // CHIAKI_DISCORDPRESENCE_H
