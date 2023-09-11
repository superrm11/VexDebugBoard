#include "HTTPLog.h"

#include <stdio.h>
#include <esp_log.h>
#include <mdns.h>

#include "freertos/semphr.h"

static const char *TAG = "web_log";

#define MUX_TIMEOUT (500 / portTICK_PERIOD_MS)

static const size_t log_grow_amt = 256;
static const size_t log_initial_cap = 256;

static const size_t max_log_length = 8192; ///< maximum size of log to avoid taking too much memory TODO scroll log rather than just cutoff
typedef struct
{
    SemaphoreHandle_t mux;
    char *str;
    int len; // what strlen would return
    int cap;
} log_internals;

log_internals web_log;

log_internals create_log_internal()
{
    SemaphoreHandle_t mux = xSemaphoreCreateMutex();

    log_internals li = {
        .mux = mux,
        .str = malloc(log_initial_cap),
        .len = 0,
        .cap = log_initial_cap};
    return li;
}



/// @brief appends a string to the log with all the safeties. External people should use log, logf type functions. Implementation of those functions call this;
/// @param str null terminated string to add to log. can be arbitrary html. Don't abuse that
static bool append_log(const char *str)
{
    if (xSemaphoreTake(web_log.mux, MUX_TIMEOUT))
    {

        int length = strlen(str);
        ESP_LOGI(TAG, "New log entry len: %d. Current log len: %d/%d", length, web_log.len,web_log.cap);
        int new_log_size = web_log.len + length;

        if (new_log_size > max_log_length)
        {
            ESP_LOGI(TAG, "log too big");
            xSemaphoreGive(web_log.mux);
            return false;
        }
        // resize if needed
        if (new_log_size > web_log.cap)
        {
            ESP_LOGI(TAG, "Growing log buffer");
            web_log.str = realloc(web_log.str, web_log.cap + log_grow_amt);
            web_log.cap = web_log.cap + log_grow_amt;
            if (web_log.str == NULL)
            {
                ESP_LOGE(TAG, "Error growing log buffer");
                xSemaphoreGive(web_log.mux);
                abort();
                return false;
            }
        }
        // strncpy((web_log.str+web_log.len), str, length);
        memcpy(web_log.str + web_log.len, str, length+1); // +1 for that null terminator
        // strncat(web_log.str, str, length);
        web_log.len = new_log_size;

        ESP_LOGI(TAG, "After log entry  Current log len: %d/%d", web_log.len,web_log.cap);


        xSemaphoreGive(web_log.mux);
    }
    return false;
}


void http_log_raw(const char * str){
    append_log(str);
}



esp_err_t get_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(web_log.mux, MUX_TIMEOUT))
    {
        ESP_LOGI(TAG, "Sending HTTP Log: %s", web_log.str);

        /* Send the log */
        httpd_resp_send(req, web_log.str, HTTPD_RESP_USE_STRLEN);

        xSemaphoreGive(web_log.mux);
        return ESP_OK;
    }
    /* Send an error response */
    const char resp[] = "<h1> Couldn't aquire log mux </h1>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t err404_handler(httpd_req_t *req)
{
    /* Send a simple response */
    ESP_LOGI(TAG, "Got 404ed");
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "404 not found.");
    return ESP_FAIL;
}

/* URI handler structure for GET /uri */
httpd_uri_t uri_get = {
    .uri = "/log",
    .method = HTTP_GET,
    .handler = get_handler,
    .user_ctx = NULL,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL,
};

/**
 * @brief Function for starting the webserver
 * @pre mDNS is initialized
 * @param port port to run server on. For browsers to see it should be 80
 */
httpd_handle_t http_log_start(uint16_t port)
{
    /* Generate default configuration */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;

    ESP_LOGI(TAG, "Starting http log server on port: '%d'", config.server_port);

    /* Initialize log */
    web_log = create_log_internal();
    append_log("<h1> Web Log </h1>\n\n<pre>");

    /* Empty handle to esp_http_server */
    httpd_handle_t server = NULL;

    /* Start the httpd server */
    if (httpd_start(&server, &config) == ESP_OK)
    {
        /* Register URI handlers */
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_get));
        // ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, &err404_handler));
        // httpd_register_uri_handler(server, &uri_post);
    }
    ESP_LOGI(TAG, "Webserver Status: %s", (server == NULL) ? "bad" : "good");
    if (server == NULL)
    {
        return server;
    }

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", port, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_instance_name_set("_http", "_tcp", "VexDebugWeb"));

    /* If server failed to start, handle will be NULL */
    return server;
}

/* Function for stopping the webserver */
void http_log_stop(httpd_handle_t server)
{
    if (server)
    {
        /* Stop the httpd server */
        httpd_stop(server);
    }
}
