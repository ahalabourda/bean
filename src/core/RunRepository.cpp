#include "core/RunRepository.h"

#include "core/SettingsStore.h"
#include "log/MythicRunDetector.h"

#include <windows.h>

#include <cstring>
#include <iomanip>
#include <sstream>

namespace bean::core {
namespace {

struct sqlite3;
struct sqlite3_stmt;

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_NULL = 5;
constexpr int SQLITE_OPEN_READONLY = 0x00000001;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
const auto SQLITE_TRANSIENT = reinterpret_cast<void(*)(void*)>(static_cast<intptr_t>(-1));

using sqlite3_open_v2_fn = int(*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_fn = int(*)(sqlite3*);
using sqlite3_exec_fn = int(*)(sqlite3*, const char*, int(*)(void*, int, char**, char**), void*, char**);
using sqlite3_free_fn = void(*)(void*);
using sqlite3_prepare_v2_fn = int(*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_bind_text_fn = int(*)(sqlite3_stmt*, int, const char*, int, void(*)(void*));
using sqlite3_bind_null_fn = int(*)(sqlite3_stmt*, int);
using sqlite3_bind_int_fn = int(*)(sqlite3_stmt*, int, int);
using sqlite3_step_fn = int(*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int(*)(sqlite3_stmt*);
using sqlite3_column_text_fn = const unsigned char*(*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int(*)(sqlite3_stmt*, int);
using sqlite3_column_type_fn = int(*)(sqlite3_stmt*, int);

struct SqliteApi {
    HMODULE dll = nullptr;
    sqlite3_open_v2_fn open_v2 = nullptr;
    sqlite3_close_fn close = nullptr;
    sqlite3_exec_fn exec = nullptr;
    sqlite3_free_fn free_fn = nullptr;
    sqlite3_prepare_v2_fn prepare_v2 = nullptr;
    sqlite3_bind_text_fn bind_text = nullptr;
    sqlite3_bind_null_fn bind_null = nullptr;
    sqlite3_bind_int_fn bind_int = nullptr;
    sqlite3_step_fn step = nullptr;
    sqlite3_finalize_fn finalize = nullptr;
    sqlite3_column_text_fn column_text = nullptr;
    sqlite3_column_int_fn column_int = nullptr;
    sqlite3_column_type_fn column_type = nullptr;
};

bool LoadSqliteApi(SqliteApi& api, std::string& error)
{
    api.dll = LoadLibraryW(L"winsqlite3.dll");
    if (!api.dll) {
        error = "Unable to load winsqlite3.dll.";
        return false;
    }

    auto load = [&](auto& fn, const char* name) -> bool {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(GetProcAddress(api.dll, name));
        if (!fn) {
            error = std::string("Missing SQLite export: ") + name;
            return false;
        }
        return true;
    };

    return load(api.open_v2, "sqlite3_open_v2")
        && load(api.close, "sqlite3_close")
        && load(api.exec, "sqlite3_exec")
        && load(api.free_fn, "sqlite3_free")
        && load(api.prepare_v2, "sqlite3_prepare_v2")
        && load(api.bind_text, "sqlite3_bind_text")
        && load(api.bind_null, "sqlite3_bind_null")
        && load(api.bind_int, "sqlite3_bind_int")
        && load(api.step, "sqlite3_step")
        && load(api.finalize, "sqlite3_finalize")
        && load(api.column_text, "sqlite3_column_text")
        && load(api.column_int, "sqlite3_column_int")
        && load(api.column_type, "sqlite3_column_type");
}

SqliteApi& GetSqliteApi()
{
    static SqliteApi api{};
    static bool loaded = false;
    static std::string loadError;
    if (!loaded) {
        LoadSqliteApi(api, loadError);
        loaded = true;
    }
    return api;
}

bool EnsureSqliteLoaded(std::string& error)
{
    auto& api = GetSqliteApi();
    if (!api.dll || !api.open_v2) {
        error = "SQLite runtime not available (winsqlite3.dll).";
        return false;
    }
    return true;
}

std::string ToIsoUtc(std::chrono::system_clock::time_point tp)
{
    const auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::optional<std::chrono::system_clock::time_point> FromIsoUtc(const char* text)
{
    if (!text || *text == '\0') {
        return std::nullopt;
    }
    std::tm tm{};
    std::istringstream is(text);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (is.fail()) {
        return std::nullopt;
    }
    const time_t t = _mkgmtime(&tm);
    if (t == -1) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(t);
}

bool ExecuteSql(sqlite3* db, const char* sql, std::string& error)
{
    auto& api = GetSqliteApi();
    char* message = nullptr;
    const int rc = api.exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) {
        return true;
    }
    error = message ? message : "sqlite3_exec failed";
    if (message) {
        api.free_fn(message);
    }
    return false;
}

void BindTextOrNull(sqlite3_stmt* stmt, int index, const std::optional<std::string>& value)
{
    auto& api = GetSqliteApi();
    if (value.has_value()) {
        api.bind_text(stmt, index, value->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        api.bind_null(stmt, index);
    }
}

void BindIntOrNull(sqlite3_stmt* stmt, int index, const std::optional<int>& value)
{
    auto& api = GetSqliteApi();
    if (value.has_value()) {
        api.bind_int(stmt, index, *value);
    } else {
        api.bind_null(stmt, index);
    }
}

bool RunParticipantsHasColumn(sqlite3* db, const char* columnName, std::string& error)
{
    auto& api = GetSqliteApi();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "PRAGMA table_info(run_participants);";
    if (api.prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
        error = "Unable to inspect run_participants schema.";
        return false;
    }

    bool found = false;
    while (api.step(stmt) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(api.column_text(stmt, 1));
        if (text && _stricmp(text, columnName) == 0) {
            found = true;
            break;
        }
    }

    api.finalize(stmt);
    return found;
}

bool MigrateRunParticipantsSchema(sqlite3* db, std::string& error)
{
    bool hasSpecName = false;
    bool hasClassName = false;
    if (!(hasSpecName = RunParticipantsHasColumn(db, "spec_name", error)) && !error.empty()) {
        return false;
    }
    if (!(hasClassName = RunParticipantsHasColumn(db, "class_name", error)) && !error.empty()) {
        return false;
    }

    if (!hasSpecName && !hasClassName) {
        return true;
    }

    if (!ExecuteSql(db, "BEGIN IMMEDIATE TRANSACTION;", error)) {
        return false;
    }

    const char* migrateSql =
        "ALTER TABLE run_participants RENAME TO run_participants_old;"
        "CREATE TABLE run_participants ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "run_id INTEGER NOT NULL,"
        "unit_guid TEXT NOT NULL,"
        "player_name TEXT,"
        "realm TEXT,"
        "region TEXT,"
        "spec_id INTEGER,"
        "FOREIGN KEY(run_id) REFERENCES runs(id) ON DELETE CASCADE"
        ");"
        "INSERT INTO run_participants (id, run_id, unit_guid, player_name, realm, region, spec_id) "
        "SELECT id, run_id, unit_guid, player_name, realm, region, spec_id FROM run_participants_old;"
        "DROP TABLE run_participants_old;"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_run_participants_run_guid ON run_participants(run_id, unit_guid);";

    if (!ExecuteSql(db, migrateSql, error)) {
        ExecuteSql(db, "ROLLBACK;", error);
        return false;
    }
    if (!ExecuteSql(db, "COMMIT;", error)) {
        ExecuteSql(db, "ROLLBACK;", error);
        return false;
    }
    return true;
}

} // namespace

RunRepository::RunRepository()
{
    SettingsStore store;
    dbPath_ = store.GetConfigPath().parent_path() / "runs.db";
}

RunRepository::RunRepository(std::filesystem::path dbPath)
    : dbPath_(std::move(dbPath))
{
}

bool RunRepository::Initialize(std::string& error)
{
    std::scoped_lock lock(mutex_);
    return EnsureInitialized(error);
}

bool RunRepository::EnsureInitialized(std::string& error)
{
    error.clear();
    if (!EnsureSqliteLoaded(error)) {
        return false;
    }
    if (initialized_) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dbPath_.parent_path(), ec);
    if (ec) {
        error = "Unable to create DB directory: " + ec.message();
        return false;
    }

    auto& api = GetSqliteApi();
    sqlite3* db = nullptr;
    const int openRc = api.open_v2(dbPath_.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (openRc != SQLITE_OK || !db) {
        error = "Unable to open runs database.";
        if (db) {
            api.close(db);
        }
        return false;
    }

    const char* schemaSql =
        "CREATE TABLE IF NOT EXISTS runs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "video_path TEXT NOT NULL UNIQUE,"
        "video_file_name TEXT NOT NULL,"
        "trigger_reason TEXT,"
        "stop_reason TEXT,"
        "result TEXT,"
        "recording_started_at_utc TEXT NOT NULL,"
        "recording_ended_at_utc TEXT NOT NULL,"
        "mythic_started_at_utc TEXT,"
        "mythic_ended_at_utc TEXT,"
        "challenge_map_id INTEGER,"
        "keystone_level INTEGER,"
        "dungeon_name TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_runs_video_file_name ON runs(video_file_name);"
        "CREATE TABLE IF NOT EXISTS run_participants ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "run_id INTEGER NOT NULL,"
        "unit_guid TEXT NOT NULL,"
        "player_name TEXT,"
        "realm TEXT,"
        "region TEXT,"
        "spec_id INTEGER,"
        "FOREIGN KEY(run_id) REFERENCES runs(id) ON DELETE CASCADE"
        ");"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_run_participants_run_guid ON run_participants(run_id, unit_guid);";
    const bool ok = ExecuteSql(db, schemaSql, error);
    if (ok && !MigrateRunParticipantsSchema(db, error)) {
        api.close(db);
        return false;
    }
    if (ok) {
        const char* manualDungeonBackfillSql =
            "UPDATE runs "
            "SET dungeon_name = 'Manual Recording' "
            "WHERE trigger_reason = 'manual' "
            "AND (dungeon_name IS NULL OR dungeon_name = '');";
        if (!ExecuteSql(db, manualDungeonBackfillSql, error)) {
            api.close(db);
            return false;
        }
    }
    api.close(db);
    if (!ok) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool RunRepository::UpsertRun(const RunRecord& record, std::string& error)
{
    std::scoped_lock lock(mutex_);
    if (!EnsureInitialized(error)) {
        return false;
    }

    auto& api = GetSqliteApi();
    sqlite3* db = nullptr;
    if (api.open_v2(dbPath_.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK || !db) {
        error = "Unable to open runs database for write.";
        if (db) {
            api.close(db);
        }
        return false;
    }
    if (!ExecuteSql(db, "PRAGMA foreign_keys = ON;", error)) {
        api.close(db);
        return false;
    }
    if (!ExecuteSql(db, "BEGIN IMMEDIATE TRANSACTION;", error)) {
        api.close(db);
        return false;
    }

    const char* sql =
        "INSERT INTO runs ("
        "video_path, video_file_name, trigger_reason, stop_reason, result,"
        "recording_started_at_utc, recording_ended_at_utc, mythic_started_at_utc, mythic_ended_at_utc,"
        "challenge_map_id, keystone_level, dungeon_name"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(video_path) DO UPDATE SET "
        "video_file_name=excluded.video_file_name,"
        "trigger_reason=excluded.trigger_reason,"
        "stop_reason=excluded.stop_reason,"
        "result=excluded.result,"
        "recording_started_at_utc=excluded.recording_started_at_utc,"
        "recording_ended_at_utc=excluded.recording_ended_at_utc,"
        "mythic_started_at_utc=excluded.mythic_started_at_utc,"
        "mythic_ended_at_utc=excluded.mythic_ended_at_utc,"
        "challenge_map_id=excluded.challenge_map_id,"
        "keystone_level=excluded.keystone_level,"
        "dungeon_name=excluded.dungeon_name;";

    sqlite3_stmt* stmt = nullptr;
    if (api.prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
        error = "Unable to prepare run upsert statement.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }

    api.bind_text(stmt, 1, record.videoPath.string().c_str(), -1, SQLITE_TRANSIENT);
    api.bind_text(stmt, 2, record.videoFileName.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 3, record.triggerReason.empty() ? std::optional<std::string>{} : std::optional<std::string>{record.triggerReason});
    BindTextOrNull(stmt, 4, record.stopReason.empty() ? std::optional<std::string>{} : std::optional<std::string>{record.stopReason});
    BindTextOrNull(stmt, 5, record.result.empty() ? std::optional<std::string>{} : std::optional<std::string>{record.result});
    const auto started = ToIsoUtc(record.recordingStartedAt);
    const auto ended = ToIsoUtc(record.recordingEndedAt);
    api.bind_text(stmt, 6, started.c_str(), -1, SQLITE_TRANSIENT);
    api.bind_text(stmt, 7, ended.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 8, record.mythicRunStartedAt.has_value() ? std::optional<std::string>{ToIsoUtc(*record.mythicRunStartedAt)} : std::nullopt);
    BindTextOrNull(stmt, 9, record.mythicRunEndedAt.has_value() ? std::optional<std::string>{ToIsoUtc(*record.mythicRunEndedAt)} : std::nullopt);
    BindIntOrNull(stmt, 10, record.challengeMapId);
    BindIntOrNull(stmt, 11, record.keystoneLevel);
    BindTextOrNull(stmt, 12, record.dungeonName);

    const int stepRc = api.step(stmt);
    api.finalize(stmt);
    if (stepRc != SQLITE_DONE) {
        error = "Failed to upsert run record.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }

    const char* findRunIdSql = "SELECT id FROM runs WHERE video_path = ?;";
    sqlite3_stmt* findRunIdStmt = nullptr;
    if (api.prepare_v2(db, findRunIdSql, -1, &findRunIdStmt, nullptr) != SQLITE_OK || !findRunIdStmt) {
        error = "Unable to prepare run id query statement.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }
    api.bind_text(findRunIdStmt, 1, record.videoPath.string().c_str(), -1, SQLITE_TRANSIENT);
    const int runIdStepRc = api.step(findRunIdStmt);
    if (runIdStepRc != SQLITE_ROW) {
        api.finalize(findRunIdStmt);
        error = "Unable to fetch run id for participant upsert.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }
    const int runId = api.column_int(findRunIdStmt, 0);
    api.finalize(findRunIdStmt);

    const char* deleteParticipantsSql = "DELETE FROM run_participants WHERE run_id = ?;";
    sqlite3_stmt* deleteStmt = nullptr;
    if (api.prepare_v2(db, deleteParticipantsSql, -1, &deleteStmt, nullptr) != SQLITE_OK || !deleteStmt) {
        error = "Unable to prepare participant delete statement.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }
    api.bind_int(deleteStmt, 1, runId);
    const int deleteStepRc = api.step(deleteStmt);
    api.finalize(deleteStmt);
    if (deleteStepRc != SQLITE_DONE) {
        error = "Failed to clear existing run participants.";
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }

    if (!record.participants.empty()) {
        const char* insertParticipantSql =
            "INSERT INTO run_participants ("
            "run_id, unit_guid, player_name, realm, region, spec_id"
            ") VALUES (?, ?, ?, ?, ?, ?);";
        for (const auto& participant : record.participants) {
            sqlite3_stmt* participantStmt = nullptr;
            if (api.prepare_v2(db, insertParticipantSql, -1, &participantStmt, nullptr) != SQLITE_OK || !participantStmt) {
                error = "Unable to prepare participant insert statement.";
                ExecuteSql(db, "ROLLBACK;", error);
                api.close(db);
                return false;
            }
            api.bind_int(participantStmt, 1, runId);
            api.bind_text(participantStmt, 2, participant.guid.c_str(), -1, SQLITE_TRANSIENT);
            BindTextOrNull(participantStmt, 3, participant.name);
            BindTextOrNull(participantStmt, 4, participant.realm);
            BindTextOrNull(participantStmt, 5, participant.region);
            BindIntOrNull(participantStmt, 6, participant.specId);

            const int participantStepRc = api.step(participantStmt);
            if (participantStepRc != SQLITE_DONE) {
                api.finalize(participantStmt);
                error = "Failed to insert run participant.";
                ExecuteSql(db, "ROLLBACK;", error);
                api.close(db);
                return false;
            }
            api.finalize(participantStmt);
        }
    }

    if (!ExecuteSql(db, "COMMIT;", error)) {
        ExecuteSql(db, "ROLLBACK;", error);
        api.close(db);
        return false;
    }

    api.close(db);
    return true;
}

std::optional<RunRecord> RunRepository::GetRunByVideoPath(const std::filesystem::path& videoPath, std::string& error)
{
    std::scoped_lock lock(mutex_);
    if (!EnsureInitialized(error)) {
        return std::nullopt;
    }

    auto& api = GetSqliteApi();
    sqlite3* db = nullptr;
    if (api.open_v2(dbPath_.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
        error = "Unable to open runs database for read.";
        if (db) {
            api.close(db);
        }
        return std::nullopt;
    }
    if (!ExecuteSql(db, "PRAGMA foreign_keys = ON;", error)) {
        api.close(db);
        return std::nullopt;
    }

    const char* sql =
        "SELECT video_path, video_file_name, trigger_reason, stop_reason, result, "
        "recording_started_at_utc, recording_ended_at_utc, mythic_started_at_utc, mythic_ended_at_utc, "
        "challenge_map_id, keystone_level, dungeon_name, id "
        "FROM runs WHERE video_path = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (api.prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
        error = "Unable to prepare run query statement.";
        api.close(db);
        return std::nullopt;
    }
    api.bind_text(stmt, 1, videoPath.string().c_str(), -1, SQLITE_TRANSIENT);

    const int stepRc = api.step(stmt);
    if (stepRc != SQLITE_ROW) {
        api.finalize(stmt);
        api.close(db);
        return std::nullopt;
    }

    RunRecord record;
    record.videoPath = reinterpret_cast<const char*>(api.column_text(stmt, 0));
    record.videoFileName = reinterpret_cast<const char*>(api.column_text(stmt, 1));
    if (const auto* text = reinterpret_cast<const char*>(api.column_text(stmt, 2)); text) {
        record.triggerReason = text;
    }
    if (const auto* text = reinterpret_cast<const char*>(api.column_text(stmt, 3)); text) {
        record.stopReason = text;
    }
    if (const auto* text = reinterpret_cast<const char*>(api.column_text(stmt, 4)); text) {
        record.result = text;
    }
    record.recordingStartedAt = FromIsoUtc(reinterpret_cast<const char*>(api.column_text(stmt, 5))).value_or(std::chrono::system_clock::time_point{});
    record.recordingEndedAt = FromIsoUtc(reinterpret_cast<const char*>(api.column_text(stmt, 6))).value_or(std::chrono::system_clock::time_point{});
    record.mythicRunStartedAt = FromIsoUtc(reinterpret_cast<const char*>(api.column_text(stmt, 7)));
    record.mythicRunEndedAt = FromIsoUtc(reinterpret_cast<const char*>(api.column_text(stmt, 8)));
    if (api.column_type(stmt, 9) != SQLITE_NULL) {
        record.challengeMapId = api.column_int(stmt, 9);
    }
    if (api.column_type(stmt, 10) != SQLITE_NULL) {
        record.keystoneLevel = api.column_int(stmt, 10);
    }
    if (const auto* text = reinterpret_cast<const char*>(api.column_text(stmt, 11)); text) {
        record.dungeonName = text;
    }
    const int runId = api.column_int(stmt, 12);

    api.finalize(stmt);

    const char* participantSql =
        "SELECT unit_guid, player_name, realm, region, spec_id "
        "FROM run_participants WHERE run_id = ? ORDER BY COALESCE(player_name, ''), unit_guid;";
    sqlite3_stmt* participantStmt = nullptr;
    if (api.prepare_v2(db, participantSql, -1, &participantStmt, nullptr) != SQLITE_OK || !participantStmt) {
        error = "Unable to prepare run participant query statement.";
        api.close(db);
        return std::nullopt;
    }
    api.bind_int(participantStmt, 1, runId);
    while (api.step(participantStmt) == SQLITE_ROW) {
        RunRecord::Participant participant;
        if (const auto* guidText = reinterpret_cast<const char*>(api.column_text(participantStmt, 0)); guidText) {
            participant.guid = guidText;
        }
        if (const auto* text = reinterpret_cast<const char*>(api.column_text(participantStmt, 1)); text) {
            participant.name = text;
        }
        if (const auto* text = reinterpret_cast<const char*>(api.column_text(participantStmt, 2)); text) {
            participant.realm = text;
        }
        if (const auto* text = reinterpret_cast<const char*>(api.column_text(participantStmt, 3)); text) {
            participant.region = text;
        }
        if (api.column_type(participantStmt, 4) != SQLITE_NULL) {
            participant.specId = api.column_int(participantStmt, 4);
            if (const auto specInfo = log::ResolveSpecInfo(*participant.specId); specInfo.has_value()) {
                participant.specName = specInfo->specName;
                participant.className = specInfo->className;
            }
        }
        if (!participant.guid.empty()) {
            record.participants.push_back(std::move(participant));
        }
    }
    api.finalize(participantStmt);

    api.close(db);
    return record;
}

std::filesystem::path RunRepository::GetDatabasePath() const
{
    return dbPath_;
}

} // namespace bean::core
