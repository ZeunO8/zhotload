/**
 * @file zhl_http_apple.m
 * @brief HTTP client implementation using Foundation's NSURLSession.
 *
 * Dependency choice: the desktop zcio backend's TLS rides OpenSSL, which the
 * iOS/tvOS/watchOS SDKs don't ship, so on Apple embedded platforms the client
 * speaks HTTP through NSURLSession — always present, https via the system
 * trust store, no third-party linkage. This is the default backend whenever
 * ZHL_HTTP_BACKEND=apple (see zhl/CMakeLists.txt).
 *
 * Both entry points are synchronous to match the zcio backend's contract:
 * the async NSURLSession task is driven to completion on a private queue and
 * awaited via a dispatch semaphore. Compiled with ARC (-fobjc-arc).
 */

#include "zhl_http.h"

#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static NSURLSession *zhl_make_session(NSTimeInterval timeout)
{
    NSURLSessionConfiguration *cfg =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = timeout;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    return [NSURLSession sessionWithConfiguration:cfg];
}

zhl_status_t zhl_http_get(const char *url, zhl_http_response_t *out)
{
    if (!url || !out) return ZHL_ERR_NULL_PARAM;

    memset(out, 0, sizeof(*out));

    @autoreleasepool {
        NSString *url_str = [NSString stringWithUTF8String:url];
        NSURL *nsurl = url_str ? [NSURL URLWithString:url_str] : nil;
        if (!nsurl) return ZHL_ERR_INVALID_URL;

        NSURLSession *session = zhl_make_session(30.0);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        __block NSData *body_data = nil;
        __block long status_code = 0;
        __block BOOL failed = NO;

        NSURLSessionDataTask *task = [session
            dataTaskWithURL:nsurl
          completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
              if (error) {
                  failed = YES;
              } else {
                  body_data = data;
                  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                      status_code = (long)[(NSHTTPURLResponse *)response statusCode];
                  }
              }
              dispatch_semaphore_signal(sem);
          }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (failed) return ZHL_ERR_NETWORK;

        size_t len = (size_t)body_data.length;
        char *buf = (char *)malloc(len + 1);
        if (!buf) return ZHL_ERR_ALLOC;
        if (len) memcpy(buf, body_data.bytes, len);
        buf[len] = '\0';

        out->status_code = status_code;
        out->body = buf;
        out->body_len = len;

        if (status_code < 200 || status_code >= 300) return ZHL_ERR_HTTP_ERROR;
        return ZHL_OK;
    }
}

zhl_status_t zhl_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return ZHL_ERR_NULL_PARAM;

    @autoreleasepool {
        NSString *url_str = [NSString stringWithUTF8String:url];
        NSURL *nsurl = url_str ? [NSURL URLWithString:url_str] : nil;
        if (!nsurl) return ZHL_ERR_INVALID_URL;

        NSString *dest_str = [NSString stringWithUTF8String:dest_path];
        NSURL *dest_url = dest_str ? [NSURL fileURLWithPath:dest_str] : nil;
        if (!dest_url) return ZHL_ERR_NULL_PARAM;

        NSURLSession *session = zhl_make_session(300.0);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        __block long status_code = 0;
        __block BOOL failed = NO;
        __block zhl_status_t move_status = ZHL_OK;

        NSURLSessionDownloadTask *task = [session
            downloadTaskWithURL:nsurl
              completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
                  if (error || !location) {
                      failed = YES;
                      dispatch_semaphore_signal(sem);
                      return;
                  }
                  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                      status_code = (long)[(NSHTTPURLResponse *)response statusCode];
                  }
                  // Only commit the temp file to its destination on a 2xx; the
                  // caller inspects status_code below and treats the rest as an
                  // HTTP error with no file written.
                  if (status_code >= 200 && status_code < 300) {
                      NSFileManager *fm = [NSFileManager defaultManager];
                      [fm removeItemAtURL:dest_url error:nil];
                      NSError *move_err = nil;
                      if (![fm moveItemAtURL:location toURL:dest_url error:&move_err]) {
                          move_status = ZHL_ERR_DISK_WRITE;
                      }
                  }
                  dispatch_semaphore_signal(sem);
              }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (failed) return ZHL_ERR_NETWORK;
        if (status_code < 200 || status_code >= 300) return ZHL_ERR_HTTP_ERROR;
        if (move_status != ZHL_OK) return move_status;
        return ZHL_OK;
    }
}

void zhl_http_response_free(zhl_http_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;
}
