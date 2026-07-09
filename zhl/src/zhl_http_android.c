/**
 * @file zhl_http_android.c
 * @brief HTTP client implementation using Android's java.net.HttpURLConnection
 *        via JNI.
 *
 * Dependency choice: the desktop zcio backend's TLS rides OpenSSL, which the
 * Android NDK doesn't ship, so — mirroring the iOS/NSURLSession backend — the
 * Android client speaks HTTP through the framework's own URL stack
 * (java.net.HttpURLConnection), which ships with every Android device, speaks
 * https via the platform trust store, and needs no third-party linkage. This is
 * the default backend whenever ZHL_HTTP_BACKEND=android (see zhl/CMakeLists.txt).
 *
 * JNI requires a JavaVM. A native static library cannot own JNI_OnLoad (that
 * belongs to the app's own .so), so the embedder hands us the VM once at startup
 * via zhl_android_init(), typically from its JNI_OnLoad. Each HTTP call attaches
 * the calling thread to the VM if needed, drives the request synchronously to
 * match the zcio/NSURLSession contract, and detaches again.
 */

#include "zhl_http.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The VM handed to us by the embedder via zhl_android_init(). */
static JavaVM *g_vm = NULL;

zhl_status_t zhl_android_init(JavaVM *vm)
{
    if (!vm) return ZHL_ERR_NULL_PARAM;
    g_vm = vm;
    return ZHL_OK;
}

/* Resolve a JNIEnv for the current thread, attaching to the VM if the thread is
 * not already attached. On success *attached tells the caller whether a matching
 * detach is owed. Returns 1 on success, 0 on failure. */
static int env_acquire(JNIEnv **env, int *attached)
{
    *env = NULL;
    *attached = 0;
    if (!g_vm) return 0;

    jint r = (*g_vm)->GetEnv(g_vm, (void **)env, JNI_VERSION_1_6);
    if (r == JNI_OK) return 1;
    if (r == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, env, NULL) == JNI_OK) {
            *attached = 1;
            return 1;
        }
    }
    return 0;
}

static void env_release(int attached)
{
    if (attached && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}

/* Clear and swallow any pending Java exception. Returns 1 if one was pending. */
static int exc_clear(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

/* Drain a java.io.InputStream to completion.
 *
 * Exactly one sink is used: if @p fp is non-NULL bytes are written to the file;
 * else if @p out_buf is non-NULL they accumulate into a freshly malloc'd,
 * NUL-terminated buffer; else the bytes are read and discarded. The stream is
 * closed before returning. */
static zhl_status_t drain_stream(JNIEnv *env, jmethodID read_mid,
                                 jmethodID close_mid, jobject stream,
                                 FILE *fp, char **out_buf, size_t *out_len)
{
    const jint CHUNK = 8192;
    zhl_status_t st = ZHL_OK;

    jbyteArray jbuf = (*env)->NewByteArray(env, CHUNK);
    if (!jbuf) return ZHL_ERR_ALLOC;

    jbyte *stage = (jbyte *)malloc((size_t)CHUNK);
    if (!stage) {
        (*env)->DeleteLocalRef(env, jbuf);
        return ZHL_ERR_ALLOC;
    }

    char  *mem = NULL;
    size_t mem_len = 0, mem_cap = 0;

    for (;;) {
        jint n = (*env)->CallIntMethod(env, stream, read_mid, jbuf);
        if (exc_clear(env)) { st = ZHL_ERR_NETWORK; break; }
        if (n < 0) break;          /* end of stream */
        if (n == 0) continue;

        (*env)->GetByteArrayRegion(env, jbuf, 0, n, stage);
        if (exc_clear(env)) { st = ZHL_ERR_NETWORK; break; }

        if (fp) {
            if (fwrite(stage, 1, (size_t)n, fp) != (size_t)n) {
                st = ZHL_ERR_DISK_WRITE;
                break;
            }
        } else if (out_buf) {
            if (mem_len + (size_t)n + 1 > mem_cap) {
                size_t new_cap = (mem_cap == 0) ? 8192 : mem_cap * 2;
                while (new_cap < mem_len + (size_t)n + 1) new_cap *= 2;
                char *tmp = (char *)realloc(mem, new_cap);
                if (!tmp) { st = ZHL_ERR_ALLOC; break; }
                mem = tmp;
                mem_cap = new_cap;
            }
            memcpy(mem + mem_len, stage, (size_t)n);
            mem_len += (size_t)n;
        }
        /* else: discard */
    }

    if (close_mid) {
        (*env)->CallVoidMethod(env, stream, close_mid);
        exc_clear(env);
    }
    free(stage);
    (*env)->DeleteLocalRef(env, jbuf);

    if (st != ZHL_OK) {
        free(mem);
        return st;
    }

    if (out_buf) {
        if (!mem) {
            mem = (char *)malloc(1);
            if (!mem) return ZHL_ERR_ALLOC;
        }
        mem[mem_len] = '\0';
        *out_buf = mem;
        *out_len = mem_len;
    }
    return ZHL_OK;
}

/* Perform a synchronous GET.
 *
 * When @p dest_path is non-NULL the 2xx body is streamed to that file (download
 * mode). Otherwise the body is buffered into @p out (in-memory GET mode). On a
 * non-2xx status the HTTP body/error stream is still consumed; in GET mode it is
 * returned in @p out just like the curl/Apple backends, and the function returns
 * ZHL_ERR_HTTP_ERROR. */
static zhl_status_t do_request(JNIEnv *env, const char *url, int timeout_ms,
                               const char *dest_path, zhl_http_response_t *out)
{
    if ((*env)->PushLocalFrame(env, 32) != 0) return ZHL_ERR_ALLOC;

    zhl_status_t st = ZHL_OK;
    long code = 0;
    jobject conn = NULL;
    FILE *fp = NULL;

    jclass url_cls  = (*env)->FindClass(env, "java/net/URL");
    jclass http_cls = (*env)->FindClass(env, "java/net/HttpURLConnection");
    jclass is_cls   = (*env)->FindClass(env, "java/io/InputStream");
    if (!url_cls || !http_cls || !is_cls) { st = ZHL_ERR_NETWORK; goto done; }

    jmethodID url_ctor  = (*env)->GetMethodID(env, url_cls, "<init>", "(Ljava/lang/String;)V");
    jmethodID open_mid  = (*env)->GetMethodID(env, url_cls, "openConnection", "()Ljava/net/URLConnection;");
    /* Timeout/stream setters live on URLConnection but are reachable through the
     * HttpURLConnection class since the returned object is one for http/https. */
    jmethodID set_ct    = (*env)->GetMethodID(env, http_cls, "setConnectTimeout", "(I)V");
    jmethodID set_rt    = (*env)->GetMethodID(env, http_cls, "setReadTimeout", "(I)V");
    jmethodID set_rm    = (*env)->GetMethodID(env, http_cls, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID set_fr    = (*env)->GetMethodID(env, http_cls, "setInstanceFollowRedirects", "(Z)V");
    jmethodID get_rc    = (*env)->GetMethodID(env, http_cls, "getResponseCode", "()I");
    jmethodID get_is    = (*env)->GetMethodID(env, http_cls, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID get_es    = (*env)->GetMethodID(env, http_cls, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID disc_mid  = (*env)->GetMethodID(env, http_cls, "disconnect", "()V");
    jmethodID read_mid  = (*env)->GetMethodID(env, is_cls, "read", "([B)I");
    jmethodID close_mid = (*env)->GetMethodID(env, is_cls, "close", "()V");
    if (!url_ctor || !open_mid || !get_rc || !get_is || !read_mid) {
        st = ZHL_ERR_NETWORK;
        goto done;
    }

    jstring jurl = (*env)->NewStringUTF(env, url);
    if (!jurl) { st = ZHL_ERR_ALLOC; goto done; }

    jobject url_obj = (*env)->NewObject(env, url_cls, url_ctor, jurl);
    if (exc_clear(env) || !url_obj) { st = ZHL_ERR_INVALID_URL; goto done; }

    conn = (*env)->CallObjectMethod(env, url_obj, open_mid);
    if (exc_clear(env) || !conn) { st = ZHL_ERR_NETWORK; goto done; }

    if (set_ct) (*env)->CallVoidMethod(env, conn, set_ct, (jint)timeout_ms);
    if (set_rt) (*env)->CallVoidMethod(env, conn, set_rt, (jint)timeout_ms);
    if (set_rm) {
        jstring m = (*env)->NewStringUTF(env, "GET");
        (*env)->CallVoidMethod(env, conn, set_rm, m);
        (*env)->DeleteLocalRef(env, m);
    }
    if (set_fr) (*env)->CallVoidMethod(env, conn, set_fr, JNI_TRUE);
    exc_clear(env);

    jint rc = (*env)->CallIntMethod(env, conn, get_rc);
    if (exc_clear(env)) { st = ZHL_ERR_NETWORK; goto disconnect; }
    code = (long)rc;

    int ok2xx = (rc >= 200 && rc < 300);

    jobject stream = NULL;
    if (ok2xx) {
        stream = (*env)->CallObjectMethod(env, conn, get_is);
        if (exc_clear(env)) stream = NULL;
    } else if (get_es) {
        stream = (*env)->CallObjectMethod(env, conn, get_es);
        if (exc_clear(env)) stream = NULL;
    }

    if (dest_path) {
        if (ok2xx) {
            if (!stream) { st = ZHL_ERR_NETWORK; goto disconnect; }
            fp = fopen(dest_path, "wb");
            if (!fp) { st = ZHL_ERR_DISK_WRITE; goto disconnect; }
            st = drain_stream(env, read_mid, close_mid, stream, fp, NULL, NULL);
            fclose(fp);
            fp = NULL;
            if (st != ZHL_OK) { remove(dest_path); goto disconnect; }
        } else if (stream) {
            /* Consume and discard the error body so the connection can be reused. */
            drain_stream(env, read_mid, close_mid, stream, NULL, NULL, NULL);
        }
    } else {
        char  *body = NULL;
        size_t body_len = 0;
        if (stream) {
            st = drain_stream(env, read_mid, close_mid, stream, NULL, &body, &body_len);
            if (st != ZHL_OK) goto disconnect;
        } else {
            body = (char *)malloc(1);
            if (!body) { st = ZHL_ERR_ALLOC; goto disconnect; }
            body[0] = '\0';
        }
        out->status_code = code;
        out->body = body;
        out->body_len = body_len;
    }

disconnect:
    if (disc_mid && conn) {
        (*env)->CallVoidMethod(env, conn, disc_mid);
        exc_clear(env);
    }
done:
    if (fp) fclose(fp);
    (*env)->PopLocalFrame(env, NULL);

    if (st != ZHL_OK) return st;
    if (code < 200 || code >= 300) return ZHL_ERR_HTTP_ERROR;
    return ZHL_OK;
}

zhl_status_t zhl_http_get(const char *url, zhl_http_response_t *out)
{
    if (!url || !out) return ZHL_ERR_NULL_PARAM;
    memset(out, 0, sizeof(*out));
    if (!g_vm) return ZHL_ERR_NOT_CONFIGURED;

    JNIEnv *env;
    int attached;
    if (!env_acquire(&env, &attached)) return ZHL_ERR_NETWORK;

    zhl_status_t st = do_request(env, url, 30000, NULL, out);
    env_release(attached);
    return st;
}

zhl_status_t zhl_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return ZHL_ERR_NULL_PARAM;
    if (!g_vm) return ZHL_ERR_NOT_CONFIGURED;

    JNIEnv *env;
    int attached;
    if (!env_acquire(&env, &attached)) return ZHL_ERR_NETWORK;

    zhl_status_t st = do_request(env, url, 300000, dest_path, NULL);
    env_release(attached);
    return st;
}

void zhl_http_response_free(zhl_http_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;
}
