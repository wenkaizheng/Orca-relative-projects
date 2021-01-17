// Created by wenkai zheng
//Server side using producer and consumer mode to receive the message and then store into DataBase.
#include <iostream>
#include <libwebsockets.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <map>
#include <sqlite3.h>
#include <pthread.h>
#include <atomic>
#include <deque>
#include "db.hpp"
#include "utils.hpp"

static payload store_data;
static int fd;
static pthread_t db_tid;
static struct lws_context *context;
static std::map<struct lws *,char*> wsi_map;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool db_get_lock_first = false;
static std::deque<char*> data_queue;
char db_name[32] = "server_db";
static db::DB db_object(db_name);
static size_t rest_result = 0;
static char* rest_buffer;
//static std::deque <char*> search_queue;
static int callback_http( struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len )
{

    switch( reason )
    {
        case LWS_CALLBACK_HTTP:
            //wsi_queue.push_back(wsi);
            printf("%s->%s\n", "LWS_CALLBACK_HTTP", FRONT_END);
            lws_serve_http_file( wsi, FRONT_END, "text/html", NULL, 0 );
            break;
        default:
            break;
    }

    return 0;
}

// len is the length for data stream
// in is the packet from one fragment
// user is used for record data
static int callback_example_server( struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len ) {
    payload *server_received_payload = (payload *) user;
    switch (reason) {
        case LWS_CALLBACK_RECEIVE: {
            char client_name[65];
            char client_ip[65];
            lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi),
                                   client_name, sizeof(client_name),
                                   client_ip, sizeof(client_ip));
            printf("SERVER_CALLBACK_RECEIVE: ip %s name %s\n", client_ip, client_name);
            if(!receive_callback(wsi, server_received_payload,in,len,1)){
                return -1;
            }
            // search op
            char search[7];
            unsigned char* p = &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]);
            if (compare_op(search,p) == 0){
                server_received_payload->op = SEARCH_OP;
                char date_buff[6];
                char sql[128];
                if (compare_date(date_buff,p+7) == 0){
                    // today case
                    char* search_date = date_copy(current_time(),0);
                    sprintf(sql, "SELECT * FROM RECORD WHERE time LIKE '%c %s %c';", 37, search_date, 37);
                    printf("%s",sql);
                    db_object.exec_db(sql,SELECT_SQL);
                    strcpy(server_received_payload->date,"today");
                    server_received_payload->cache_found = false;
                    free(search_date);
                }
                // Jan/9/2021
                //char sql[128];
                else {
                    if (!db_object.contains_cache(p+7)){
                        sprintf(sql, "SELECT * FROM RECORD WHERE time LIKE '%c %s %c';", 37, p + 7, 37);
                        db_object.exec_db(sql,SELECT_SQL);
                        strcpy(server_received_payload->date,(char*) p+7);
                        server_received_payload->cache_found = false;
                    }
                    else{
                        printf("Using cached data in database\n");
                        strcpy(server_received_payload->date,(char*) p+7);
                        server_received_payload->cache_found = true;
                    }
                }

                lws_callback_on_writable(wsi);
                break;
            }
            char time[65];
            char real_data[EXAMPLE_RX_BUFFER_BYTES];
            separate_data(p,time,real_data);
            char name[64];
            name_copy(name,real_data);
            // found
            if(wsi_map.find(wsi)!=wsi_map.end()){
                 // same ws (web socket)
            }
            // not found
            else{
                std::map<struct lws*,char*>::iterator itr;
                for (itr = wsi_map.begin(); itr != wsi_map.end(); ++itr) {
                    // not allow the same id;
                    if (strcmp(name,itr->second) == 0){
                        return -1;
                     }
                }
                wsi_map[wsi] = strdup(name);
            }
            store_data = *server_received_payload;
            server_received_payload->op = NORMAL_OP;
            lws_callback_on_writable_all_protocol(lws_get_context(wsi), lws_get_protocol(wsi));
            // producer
            pthread_mutex_lock(&mtx);
            if (db_get_lock_first) {
                pthread_cond_signal(&cond);
            }
            // cache the data for store in db
            data_queue.push_back(strdup(time));
            data_queue.push_back(strdup(real_data));
            pthread_mutex_unlock(&mtx);
            break;
        }
        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (server_received_payload->op == SEARCH_OP) {
                if (rest_result !=0){
                    unsigned char *real_write_back = &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]);
                    if (rest_result > EXAMPLE_RX_BUFFER_BYTES){
                        printf("Server chunks the packet\n");
                        memcpy(real_write_back,rest_buffer,EXAMPLE_RX_BUFFER_BYTES);
                        lws_write(wsi, &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]),
                                  EXAMPLE_RX_BUFFER_BYTES,
                                  LWS_WRITE_TEXT);
                        rest_result -= EXAMPLE_RX_BUFFER_BYTES;
                        rest_buffer += EXAMPLE_RX_BUFFER_BYTES;
                        bzero(real_write_back,EXAMPLE_RX_BUFFER_BYTES);
                        lws_callback_on_writable(wsi);
                        break;
                    }else{
                        printf("150th\n");
                        memcpy(real_write_back,rest_buffer,rest_result);
                        lws_write(wsi, &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]),
                                  rest_result,
                                  LWS_WRITE_TEXT);
                        bzero(real_write_back,rest_result);
                        rest_result = 0;
                        free(rest_buffer);
                        break;
                    }
                }
                std::vector<char *> search_queue;
                if(server_received_payload -> cache_found ){
                    search_queue = db_object.get_cache();
                    printf("163th %ld------\n",search_queue.size());
                }
                else{
                    search_queue = db_object.get_answer();
                }
                // we count the total length first
                size_t write_back_len = 0;
                unsigned char *real_write_back = &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]);
                for (char *ch : search_queue) {
                    write_back_len += strlen(ch);
                }
                unsigned char write_back_buff[EXAMPLE_RX_BUFFER_BYTES + write_back_len];
                unsigned char* write_back = write_back_buff;
                for (char *ch : search_queue) {
                    memcpy(write_back, ch, strlen(ch));
                    write_back += strlen(ch);
                }
                // move to the begin
                write_back -=write_back_len;
                if (write_back_len > EXAMPLE_RX_BUFFER_BYTES){
                           memcpy(real_write_back, write_back, EXAMPLE_RX_BUFFER_BYTES);
                           write_back += EXAMPLE_RX_BUFFER_BYTES;
                           write_back_len -= EXAMPLE_RX_BUFFER_BYTES;
                           lws_write(wsi, &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]),
                                     EXAMPLE_RX_BUFFER_BYTES,
                                     LWS_WRITE_TEXT);
                           // set for next chunk
                           rest_buffer = strdup((char*)write_back);
                           rest_result = write_back_len;
                           printf("190th %ld\n",rest_result);
                           bzero(real_write_back,EXAMPLE_RX_BUFFER_BYTES);
                           lws_callback_on_writable(wsi);

                }
                else {
                    memcpy(real_write_back,write_back,write_back_len);
                    lws_write(wsi, &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]),
                              write_back_len,
                              LWS_WRITE_TEXT);
                }
                if(server_received_payload->cache_found){
                    db_object.set_cache_empty();
                }
              //  printf("157th %s\n",server_received_payload->date);
                if (!server_received_payload->cache_found) {
                    db_object.set_answer_empty(server_received_payload->date);
                }
                printf("156th\n");
            }
            else {
                if (len>0) {
                    // used ws cache sessions data directly
                    printf("Server callback: %s-> %s", "LWS_CALLBACK_SERVER_WRITEABLE",
                           &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]));
                    lws_write(wsi, &(server_received_payload->data[LWS_SEND_BUFFER_PRE_PADDING]),
                              server_received_payload->len,
                              LWS_WRITE_TEXT);

                }
                else {
                    // http writeable
                    printf("Server callback: %s-> %s", "LWS_CALLBACK_SERVER_WRITEABLE",
                           &(store_data.data[LWS_SEND_BUFFER_PRE_PADDING]));
                    lws_write(wsi, &(store_data.data[LWS_SEND_BUFFER_PRE_PADDING]), store_data.len,
                              LWS_WRITE_TEXT);
                }
            }
                break;

        // when a client disconnect
        case LWS_CALLBACK_CLOSED:
            printf("Client callback: %s\n", "LWS_CALLBACK_CLOSE");
            wsi_map.erase(wsi);
        default:
            break;
    }

    return 0;
}

void shutdown(int signo) {
    std::map<struct lws*,char*>::iterator itr;
    for (itr = wsi_map.begin(); itr != wsi_map.end(); ++itr) {
        free(itr->second);
    }
    db_object.close_db();
    lws_context_destroy(context);
    exit(0);
}
static struct lws_protocols server_protocols[] =
        {
                /* The first protocol must always be the HTTP handler */
                {
                        "http-only",   /* name */
                        callback_http, /* callback */
                              0,             /* per session data space. */
                                 0,             /* max frame size / rx buffer */
                },
                {
                        "ws-protocol-example",
                        callback_example_server,
                              sizeof(payload),
                        EXAMPLE_RX_BUFFER_BYTES,
                },
                { NULL, NULL, 0, 0 } /* terminator */
        };
void* db_thread(void* vargp) {
    db_object.open_db();
    db_object.create_table(CREATE_TABLE);
    while (1) {
        // consumer
        pthread_mutex_lock(&mtx);
        if (data_queue.empty()) {
            db_get_lock_first = true;
            pthread_cond_wait(&cond, &mtx);
        }
        char* time = data_queue.front();
        data_queue.pop_front();
        char* data = data_queue.front();
        data_queue.pop_front();
        db_get_lock_first = false;
        pthread_mutex_unlock(&mtx);

        char sql[128];
        char* justified_time = date_copy(time,1);
        sprintf(sql, "INSERT INTO RECORD (TIME,MESSAGE) "  \
        "VALUES ('%s', '%s'); ", justified_time,data);
        db_object.exec_db(sql,INSERT_SQL);
        free(data);
        free(justified_time);

    }
    return NULL;
}
int main()
{
    signal(SIGINT,shutdown);
    struct lws_context_creation_info info;
    memset( &info, 0, sizeof(info) );

    info.port = 8000;
    info.protocols = server_protocols;
    info.gid = -1;
    info.uid = -1;

    context  = lws_create_context( &info );
    printf("websockets server starts at %d %p\n", info.port, context);
    int stop_server = 0;
    pthread_create(&db_tid,NULL, db_thread, NULL);
    while( !stop_server )
    {
        lws_service( context, /* timeout_ms = */ 0 );
    }
}