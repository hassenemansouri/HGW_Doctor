#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "logger.h"
#include "uploader.h"

static UploaderConfig       s_cfg;
static upload_done_callback s_callback = NULL;
static void                *s_userdata = NULL;
static int                  s_curl_ready = 0;

int uploader_init(const UploaderConfig *cfg, upload_done_callback cb, void *userdata) {
    if (!cfg) return -1;

    s_cfg = *cfg;
    s_callback = cb;
    s_userdata = userdata;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return -1;
    s_curl_ready = 1;
    return 0;
}

int uploader_send(const char *archive_path) {
    CURL *curl;
    curl_mime *mime;
    curl_mimepart *part;
    UploadStatus status = UPLOAD_STATUS_FAILED;
    int attempt;

    if (!s_curl_ready || !archive_path || archive_path[0] == '\0' || s_cfg.url[0] == '\0') {
        if (s_callback) s_callback(UPLOAD_STATUS_FAILED, archive_path, s_userdata);
        return -1;
    }

    for (attempt = 0; attempt <= (int) s_cfg.max_retries; ++attempt) {
        CURLcode rc;
        long http_code = 0;

        curl = curl_easy_init();
        if (!curl) break;

        mime = curl_mime_init(curl);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, archive_path);

        curl_easy_setopt(curl, CURLOPT_URL, s_cfg.url);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) s_cfg.timeout_s);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "hgw-doctor/0.1");

        if (!s_cfg.tls_verify) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        } else if (s_cfg.ca_cert_path[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_CAINFO, s_cfg.ca_cert_path);
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
                 attempt + 1, archive_path, rc, http_code);
        if (attempt < (int) s_cfg.max_retries && s_cfg.retry_delay_s > 0) {
            sleep(s_cfg.retry_delay_s);
        }
    }

    if (status == UPLOAD_STATUS_SUCCESS) {
        LOG_INFO("Upload successful for %s", archive_path);
    }
    if (s_callback) s_callback(status, archive_path, s_userdata);
    return (status == UPLOAD_STATUS_SUCCESS) ? 0 : -1;
}

void uploader_cleanup(void) {
    if (s_curl_ready) curl_global_cleanup();
    s_curl_ready = 0;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_callback = NULL;
    s_userdata = NULL;
}