#include <android/log.h>
#include <cstdint>
#include <cstring>

// LEGACY1_RA_HTTPS_V1
#ifndef RC_CLIENT_SUPPORTS_HASH
#define RC_CLIENT_SUPPORTS_HASH
#endif
#include "rc_client.h"
#include "rc_consoles.h"
#include <unordered_map>
#include <vector>
#include <jni.h>
#include <mutex>
#include <string>

// LEGACY1_RA_MEMORY_BRIDGE
// Read-only RetroAchievements memory access.
// This bridge does not yet authenticate or submit achievements.
namespace {

const std::uint8_t* current_iwram = nullptr;
const std::uint8_t* current_ewram = nullptr;
rc_client_t* client = nullptr;
const std::uint8_t* current_rom = nullptr;
std::size_t current_rom_size = 0;
bool login_started = false;
bool ra_game_verified = false;

JavaVM* ra_vm = nullptr;
jclass ra_http_class = nullptr;
jmethodID ra_http_method = nullptr;
jmethodID ra_overlay_method = nullptr; // LEGACY1_RA_OVERLAY_V1

struct HttpCallback {
    rc_client_server_callback_t callback;
    void* data;
};

struct HttpResult {
    HttpCallback target;
    int status;
    std::string body;
};

std::mutex http_mutex;
std::uint64_t next_http_id = 1;
std::unordered_map<std::uint64_t, HttpCallback> requests;
std::vector<HttpResult> responses;

// LEGACY1_RA_SESSION_BRIDGE_V1
std::mutex session_mutex;
std::string session_user;
std::string session_token;
bool session_hardcore_preferred = true;

std::uint32_t read_memory(
    std::uint32_t address,
    std::uint8_t* output,
    std::uint32_t bytes,
    rc_client_t*) {

    if (!current_iwram || !current_ewram || !output)
        return 0;

    const std::uint32_t requested = bytes;

    while (bytes > 0) {
        const std::uint8_t* source = nullptr;
        std::uint32_t available = 0;

        if (address < 0x8000u) {
            source = current_iwram + address;
            available = 0x8000u - address;
        } else if (address < 0x48000u) {
            source = current_ewram + (address - 0x8000u);
            available = 0x48000u - address;
        } else {
            // Cartridge save RAM will be connected separately.
            return 0;
        }

        const auto count =
            bytes < available ? bytes : available;

        std::memcpy(output, source, count);
        address += count;
        output += count;
        bytes -= count;
    }

    return requested;
}

// Deliberately inactive until the authenticated HTTPS
// transport is implemented. No unlocks are submitted.

void complete_http(
    std::uint64_t id, int status, std::string body) {

    std::lock_guard<std::mutex> lock(http_mutex);
    const auto it = requests.find(id);
    if (it == requests.end()) return;

    responses.push_back({it->second, status, std::move(body)});
    requests.erase(it);
}

void server_https(
    const rc_api_request_t* request,
    rc_client_server_callback_t callback,
    void* callback_data,
    rc_client_t*) {

    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(http_mutex);
        id = next_http_id++;
        requests.emplace(id, HttpCallback{callback, callback_data});
    }

    if (!ra_vm || !ra_http_class || !ra_http_method ||
        !request || !request->url) {
        complete_http(id, 503, "");
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;

    if (ra_vm->GetEnv(reinterpret_cast<void**>(&env),
                      JNI_VERSION_1_6) != JNI_OK) {
        if (ra_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            complete_http(id, 503, "");
            return;
        }
        attached = true;
    }

    jstring url = env->NewStringUTF(request->url);
    jstring post = env->NewStringUTF(
        request->post_data ? request->post_data : "");
    jstring type = env->NewStringUTF(
        request->content_type ? request->content_type : "");

    bool failed = !url || !post || !type;

    if (!failed) {
        env->CallStaticVoidMethod(
            ra_http_class, ra_http_method,
            static_cast<jlong>(id), url, post, type);
        failed = env->ExceptionCheck();
    }

    if (env->ExceptionCheck()) env->ExceptionClear();

    if (url) env->DeleteLocalRef(url);
    if (post) env->DeleteLocalRef(post);
    if (type) env->DeleteLocalRef(type);

    if (attached) ra_vm->DetachCurrentThread();

    if (failed) complete_http(id, 503, "");
}

void drain_http_responses() {
    std::vector<HttpResult> ready;
    {
        std::lock_guard<std::mutex> lock(http_mutex);
        ready.swap(responses);
    }

    // All rc_client callbacks run on the emulation thread.
    for (const auto& item : ready) {
        rc_api_server_response_t response{};
        response.body = item.body.c_str();
        response.body_length = item.body.size();
        response.http_status_code = item.status;
        item.target.callback(&response, item.target.data);
    }
}

// LEGACY1_RA_SPECTATOR_V1
// Eventos reales, sin registrar desbloqueos en el servidor.
// LEGACY1_RA_LIVE_SOFTCORE_V1
// Official Softcore submissions enabled.
// Hardcore remains disabled until RA client approval.

// Delivers a real achievement event to the upper Android Activity.
void show_android_achievement(const rc_client_achievement_t* achievement) {
    if (!achievement || !ra_vm ||
        !ra_http_class || !ra_overlay_method)
        return;

    JNIEnv* env = nullptr;
    bool attached = false;

    if (ra_vm->GetEnv(reinterpret_cast<void**>(&env),
                      JNI_VERSION_1_6) != JNI_OK) {
        if (ra_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return;
        attached = true;
    }

    char badge_url[512] = {};
    if (rc_client_achievement_get_image_url(
            achievement,
            RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,
            badge_url, sizeof(badge_url)) != RC_OK) {
        badge_url[0] = '\0';
    }

    jstring title = env->NewStringUTF(
        achievement->title ? achievement->title : "Logro");
    jstring desc = env->NewStringUTF(
        achievement->description ? achievement->description : "");
    jstring badge = env->NewStringUTF(badge_url);

    if (title && desc && badge) {
        env->CallStaticVoidMethod(
            ra_http_class, ra_overlay_method,
            title, desc,
            static_cast<jint>(achievement->points),
            badge);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(
            ANDROID_LOG_ERROR, "LEGACY1_RA",
            "OVERLAY_DISPATCH_ERROR");
    }

    if (title) env->DeleteLocalRef(title);
    if (desc) env->DeleteLocalRef(desc);
    if (badge) env->DeleteLocalRef(badge);
    if (attached) ra_vm->DetachCurrentThread();
}

void achievement_event_online(
    const rc_client_event_t* event, rc_client_t*) {

    if (!event) return;

    if (event->type == RC_CLIENT_EVENT_SERVER_ERROR) {
        const auto* error = event->server_error;
        __android_log_print(
            ANDROID_LOG_ERROR, "LEGACY1_RA",
            "SERVER_ERROR result=%d related_id=%u api=%s",
            error ? error->result : 0,
            error ? error->related_id : 0,
            error && error->api ? error->api : "unknown");
        return;
    }

    if (event->type == RC_CLIENT_EVENT_DISCONNECTED) {
        __android_log_print(
            ANDROID_LOG_WARN, "LEGACY1_RA",
            "SERVER_DISCONNECTED: envio pendiente");
        return;
    }

    if (event->type == RC_CLIENT_EVENT_RECONNECTED) {
        __android_log_print(
            ANDROID_LOG_INFO, "LEGACY1_RA",
            "SERVER_RECONNECTED");
        return;
    }

    if (event->type != RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED ||
        !event->achievement)
        return;

    const auto* achievement = event->achievement;

    if (achievement->id == 101000001u) {
        __android_log_print(
            ANDROID_LOG_WARN, "LEGACY1_RA",
            "SERVER_WARNING: Unknown Emulator");
        return;
    }

    __android_log_print(
        ANDROID_LOG_INFO, "LEGACY1_RA",
        "ACHIEVEMENT_TRIGGERED id=%u points=%u title=%s",
        achievement->id,
        achievement->points,
        achievement->title ? achievement->title : "Unknown");

    show_android_achievement(achievement);
}

void game_loaded(
    int result, const char*, rc_client_t* active, void*) {

    const rc_client_game_t* game =
        result == RC_OK ? rc_client_get_game_info(active) : nullptr;

    ra_game_verified =
        result == RC_OK &&
        game &&
        game->id == 758 &&
        game->console_id == RC_CONSOLE_GAMEBOY_ADVANCE;

    __android_log_print(
        ra_game_verified ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "LEGACY1_RA",
        "GAME_VERIFY %s spectator=0",
        ra_game_verified ? "OK" : "ERROR");

    __android_log_print(
        result == RC_OK && game && game->id != 0
            ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "LEGACY1_RA",
        "GAME_LOAD %s result=%d game_id=%u console=%u title=%s",
        result == RC_OK ? "OK" : "ERROR",
        result,
        game ? game->id : 0,
        game ? game->console_id : 0,
        game && game->title ? game->title : "unknown");
}

void login_completed(
    int result, const char*, rc_client_t* active, void*) {

    __android_log_print(
        result == RC_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "LEGACY1_RA",
        "RC_LOGIN %s result=%d",
        result == RC_OK ? "OK" : "ERROR", result);

    if (result != RC_OK || !current_rom)
        return;

    rc_client_begin_identify_and_load_game(
        active,
        RC_CONSOLE_GAMEBOY_ADVANCE,
        "legacy1-europe.gba",
        current_rom,
        current_rom_size,
        game_loaded,
        nullptr);
}

} // namespace

extern "C" void legacy1_ra_memory_frame(
    const std::uint8_t* iwram,
    const std::uint8_t* ewram,
    const std::uint8_t* rom,
    std::size_t rom_size) {

    if (!iwram || !ewram || !rom ||
        rom_size != 0x800000u ||
        std::memcmp(rom + 0xAC, "ALGP", 4) != 0)
        return;

    current_iwram = iwram;
    current_ewram = ewram;
    current_rom = rom;
    current_rom_size = rom_size;

    if (!client) {
        client = rc_client_create(
            read_memory, server_https);

        if (!client) {
            __android_log_print(ANDROID_LOG_ERROR,
                "LEGACY1_RA", "No se pudo crear rc_client");
            return;
        }

        // Disable server-side Hardcore claims until the
        // client has been validated and approved.
        rc_client_set_hardcore_enabled(client, 0);
        rc_client_set_spectator_mode_enabled(client, 0);
        rc_client_set_event_handler(
            client, achievement_event_online);

        __android_log_print(
            ANDROID_LOG_INFO,
            "LEGACY1_RA",
            "SOFTCORE_ONLINE hardcore=0 submissions=enabled");
        rc_client_set_allow_background_memory_reads(client, 0);

        std::uint8_t skill_flags = 0;
        std::uint8_t ewram_byte = 0;

        const bool mapping_ok =
            read_memory(0x4D, &skill_flags, 1, client) == 1 &&
            skill_flags == iwram[0x4D] &&
            read_memory(0x8000, &ewram_byte, 1, client) == 1 &&
            ewram_byte == ewram[0];

        __android_log_print(
            mapping_ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "LEGACY1_RA",
            "MEMORY_BRIDGE %s: IWRAM + EWRAM; "
            "login y logros pendientes",
            mapping_ok ? "OK" : "ERROR");
    }

    // Currently no game session has been loaded, so no
    // achievements are evaluated or submitted.
    drain_http_responses();

    if (!login_started) {
        std::string user;
        std::string token;
        {
            std::lock_guard<std::mutex> lock(session_mutex);
            user = session_user;
            token = session_token;
        }

        if (!user.empty() && !token.empty()) {
            login_started = true;
            __android_log_print(
                ANDROID_LOG_INFO, "LEGACY1_RA",
                "RC_LOGIN START");

            rc_client_begin_login_with_token(
                client, user.c_str(), token.c_str(),
                login_completed, nullptr);
        }
    }

    // Load account and game metadata, but do not evaluate
    // or submit achievements until SRAM and policy checks
    // are completed.
    if (ra_game_verified) {
        rc_client_do_frame(client);
    } else {
        rc_client_idle(client);
    }
}


// Called by GbaGameActivity once SDL has loaded the native library.
extern "C" JNIEXPORT void JNICALL
Java_org_gbarecomp_GbaNative_setRetroAchievementsSession(
    JNIEnv* env, jclass, jstring username,
    jstring token, jboolean hardcore) {

    if (!username || !token) return;

    // Resolve the Android HTTPS worker from a Java-owned thread.
    if (!ra_vm) env->GetJavaVM(&ra_vm);

    if (!ra_http_class) {
        jclass local = env->FindClass(
            "org/gbarecomp/Legacy1RaHttp");

        if (local) {
            ra_http_class = static_cast<jclass>(
                env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        }

        if (ra_http_class) {
            ra_http_method = env->GetStaticMethodID(
                ra_http_class, "request",
                "(JLjava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;)V");
            ra_overlay_method = env->GetStaticMethodID(
                ra_http_class, "achievementUnlocked",
                "(Ljava/lang/String;Ljava/lang/String;"
                "ILjava/lang/String;)V");
        }

        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    const char* u = env->GetStringUTFChars(username, nullptr);
    if (!u) return;

    const char* t = env->GetStringUTFChars(token, nullptr);
    if (!t) {
        env->ReleaseStringUTFChars(username, u);
        return;
    }

    bool ready;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        session_user.assign(u);
        session_token.assign(t);
        session_hardcore_preferred = hardcore == JNI_TRUE;
        ready = !session_user.empty() && !session_token.empty();
    }

    env->ReleaseStringUTFChars(token, t);
    env->ReleaseStringUTFChars(username, u);

    __android_log_print(
        ANDROID_LOG_INFO, "LEGACY1_RA",
        "SESSION_BRIDGE %s; hardcore_preference=%d",
        ready ? "READY" : "NO_ACCOUNT",
        hardcore == JNI_TRUE ? 1 : 0);
}


// Network replies are only queued here; rc_client is never
// invoked from an Android network worker thread.
extern "C" JNIEXPORT void JNICALL
Java_org_gbarecomp_GbaNative_completeRaHttp(
    JNIEnv* env, jclass, jlong id, jint status, jstring body) {

    std::string text;

    if (body) {
        const char* chars = env->GetStringUTFChars(body, nullptr);
        if (chars) {
            text.assign(chars);
            env->ReleaseStringUTFChars(body, chars);
        }
    }

    complete_http(
        static_cast<std::uint64_t>(id),
        static_cast<int>(status),
        std::move(text));
}
