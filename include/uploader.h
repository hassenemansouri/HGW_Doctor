#ifndef HGW_UPLOADER_H
#define HGW_UPLOADER_H

#include <stdint.h>

#include "types.h"

typedef struct {
    char     url[HGW_MAX_URL];
    uint32_t timeout_s;
    uint32_t max_retries;
    uint32_t retry_delay_s;
    bool     tls_verify;
    char     ca_cert_path[HGW_MAX_PATH];
} UploaderConfig;

typedef void (*upload_done_callback)(UploadStatus status, const char *path,
                                     void *userdata);

int uploader_init(const UploaderConfig *cfg, upload_done_callback cb, void *userdata);
int uploader_send(const char *archive_path);
void uploader_cleanup(void);

#endif /* HGW_UPLOADER_H */