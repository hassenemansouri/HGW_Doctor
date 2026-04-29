#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "logger.h"
#include "uploader.h"

static UploaderConfig       s_cfg;
static upload_done_callback s_callback = NULL;
static void                *s_userdata = NULL;
static int                  s_curl_ready = 0;

/* Upload worker thread state */
static pthread_t            s_upload_thread;
static int                  s_thread_active = 0;
static pthread_mutex_t      s_thread_mutex  = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char          path[HGW_MAX_PATH * 2];
    UploaderConfig cfg;   /* snapshot taken at dispatch time — thread-safe */
} UploadJob;

static void *upload_thread_fn(void *arg) {
    UploadJob *job = (UploadJob *)arg;
    CURL *curl;
    curl_mime *mime;
    curl_mimepart *part;
    UploadStatus status = UPLOAD_STATUS_FAILED;

    for (int attempt = 0; attempt <= (int)job->cfg.max_retries; ++attempt) {
        CURLcode rc;
        long http_code = 0;

        curl = curl_easy_init();
        if (!curl) break;

        mime = curl_mime_init(curl);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, job->path);

        curl_easy_setopt(curl, CURLOPT_URL, job->cfg.url);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)job->cfg.timeout_s);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "hgw-doctor/0.1");

        if (!job->cfg.tls_verify) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        } else if (job->cfg.ca_cert_path[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_CAINFO, job->cfg.ca_cert_path);
        }

        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_mime_free(mime);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
            status = UPLOAD_STATUS_SUCCESS;
            break;
        }

        LOG_WARN("Upload attempt %d failed for %s (curl=%d http=%ld)",
                 attempt + 1, job->path, rc, http_code);
        if (attempt < (int)job->cfg.max_retries && job->cfg.retry_delay_s > 0)
            sleep(job->cfg.retry_delay_s);
    }

    if (status == UPLOAD_STATUS_SUCCESS)
        LOG_INFO("Upload successful for %s", job->path);

    if (s_callback) s_callback(status, job->path, s_userdata);

    free(job);

    /* Mark inactive last — cleanup busy-waits on this flag */
    pthread_mutex_lock(&s_thread_mutex);
    s_thread_active = 0;
    pthread_mutex_unlock(&s_thread_mutex);

    return NULL;
}

int uploader_init(const UploaderConfig *cfg, upload_done_callback cb, void *userdata) {
    if (!cfg) return -1;

    s_cfg      = *cfg;
    s_callback = cb;
    s_userdata = userdata;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return -1;
    s_curl_ready = 1;
    return 0;
}

int uploader_send(const char *archive_path) {
    if (!s_curl_ready || !archive_path || archive_path[0] == '\0') {
        if (s_callback) s_callback(UPLOAD_STATUS_FAILED, archive_path, s_userdata);
        return -1;
    }

    pthread_mutex_lock(&s_thread_mutex);
    /* Check url under mutex — uploader_update_url/config write it under the same lock */
    if (s_cfg.url[0] == '\0') {
        pthread_mutex_unlock(&s_thread_mutex);
        if (s_callback) s_callback(UPLOAD_STATUS_FAILED, archive_path, s_userdata);
        return -1;
    }
    if (s_thread_active) {
        pthread_mutex_unlock(&s_thread_mutex);
        LOG_WARN("Upload already in progress, dropping: %s", archive_path);
        return -1;
    }

    UploadJob *job = malloc(sizeof(UploadJob));
    if (!job) {
        pthread_mutex_unlock(&s_thread_mutex);
        if (s_callback) s_callback(UPLOAD_STATUS_FAILED, archive_path, s_userdata);
        return -1;
    }
    strncpy(job->path, archive_path, sizeof(job->path) - 1);
    job->path[sizeof(job->path) - 1] = '\0';
    job->cfg = s_cfg;  /* snapshot under mutex — thread reads job->cfg, never s_cfg */

    s_thread_active = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&s_upload_thread, &attr, upload_thread_fn, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        s_thread_active = 0;
        pthread_mutex_unlock(&s_thread_mutex);
        free(job);
        if (s_callback) s_callback(UPLOAD_STATUS_FAILED, archive_path, s_userdata);
        return -1;
    }
    pthread_mutex_unlock(&s_thread_mutex);

    LOG_INFO("Upload started in background for %s", archive_path);
    return 0;
}

void uploader_update_url(const char *url) {
    if (!url || url[0] == '\0') return;
    pthread_mutex_lock(&s_thread_mutex);
    strncpy(s_cfg.url, url, sizeof(s_cfg.url) - 1);
    s_cfg.url[sizeof(s_cfg.url) - 1] = '\0';
    pthread_mutex_unlock(&s_thread_mutex);
    LOG_INFO("Upload URL updated to: %s", url);
}

void uploader_update_config(const UploaderConfig *cfg) {
    if (!cfg) return;
    pthread_mutex_lock(&s_thread_mutex);
    s_cfg = *cfg;
    pthread_mutex_unlock(&s_thread_mutex);
    LOG_INFO("Uploader config updated");
}

void uploader_cleanup(void) {
    /* Upload thread is detached — busy-wait for it to finish (up to 30s) */
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&s_thread_mutex);
        int active = s_thread_active;
        pthread_mutex_unlock(&s_thread_mutex);
        if (!active) break;
        if (i == 0) LOG_INFO("Waiting for upload thread to finish...");
        usleep(100000);
    }

    if (s_curl_ready) curl_global_cleanup();
    s_curl_ready = 0;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
    pthread_mutex_destroy(&s_thread_mutex);
}
