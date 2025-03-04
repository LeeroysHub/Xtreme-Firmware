#include "flipper.pb.h"
#include <core/common_defines.h>
#include <core/memmgr.h>
#include <core/record.h>
#include "pb_decode.h"
#include "rpc/rpc.h"
#include "rpc_i.h"
#include "storage.pb.h"
#include "storage/filesystem_api_defines.h"
#include "storage/storage.h"
#include <stdint.h>
#include <lib/toolbox/md5_calc.h>
#include <lib/toolbox/path.h>
#include <update_util/lfs_backup.h>

#define TAG "RpcStorage"

#define MAX_NAME_LENGTH 254

static const size_t MAX_DATA_SIZE = 512;

typedef enum {
    RpcStorageStateIdle = 0,
    RpcStorageStateWriting,
} RpcStorageState;

typedef struct {
    RpcSession* session;
    Storage* api;
    File* file;
    RpcStorageState state;
    uint32_t current_command_id;
} RpcStorageSystem;

static void rpc_system_storage_reset_state(
    RpcStorageSystem* rpc_storage,
    RpcSession* session,
    bool send_error) {
    furi_assert(rpc_storage);
    furi_assert(session);

    if(rpc_storage->state != RpcStorageStateIdle) {
        if(send_error) {
            rpc_send_and_release_empty(
                session,
                rpc_storage->current_command_id,
                PB_CommandStatus_ERROR_CONTINUOUS_COMMAND_INTERRUPTED);
        }

        if(rpc_storage->state == RpcStorageStateWriting) {
            storage_file_close(rpc_storage->file);
            storage_file_free(rpc_storage->file);
            furi_record_close(RECORD_STORAGE);
        }

        rpc_storage->state = RpcStorageStateIdle;
    }
}

PB_CommandStatus rpc_system_storage_get_error(FS_Error fs_error) {
    PB_CommandStatus pb_error;
    switch(fs_error) {
    case FSE_OK:
        pb_error = PB_CommandStatus_OK;
        break;
    case FSE_INVALID_NAME:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
        break;
    case FSE_INVALID_PARAMETER:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INVALID_PARAMETER;
        break;
    case FSE_INTERNAL:
        pb_error = PB_CommandStatus_ERROR_STORAGE_INTERNAL;
        break;
    case FSE_ALREADY_OPEN:
        pb_error = PB_CommandStatus_ERROR_STORAGE_ALREADY_OPEN;
        break;
    case FSE_DENIED:
        pb_error = PB_CommandStatus_ERROR_STORAGE_DENIED;
        break;
    case FSE_EXIST:
        pb_error = PB_CommandStatus_ERROR_STORAGE_EXIST;
        break;
    case FSE_NOT_EXIST:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_EXIST;
        break;
    case FSE_NOT_READY:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_READY;
        break;
    case FSE_NOT_IMPLEMENTED:
        pb_error = PB_CommandStatus_ERROR_STORAGE_NOT_IMPLEMENTED;
        break;
    default:
        pb_error = PB_CommandStatus_ERROR;
        break;
    }

    return pb_error;
}

static PB_CommandStatus rpc_system_storage_get_file_error(File* file) {
    return rpc_system_storage_get_error(storage_file_get_error(file));
}

static void rpc_system_storage_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_info_request_tag);

    FURI_LOG_D(TAG, "Info");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    FS_Error error = storage_common_fs_info(
        fs_api,
        request->content.storage_info_request.path,
        &response->content.storage_info_response.total_space,
        &response->content.storage_info_response.free_space);

    response->command_status = rpc_system_storage_get_error(error);
    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_info_response_tag;
    } else {
        response->which_content = PB_Main_empty_tag;
    }

    rpc_send_and_release(session, response);
    free(response);
    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_timestamp_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_timestamp_request_tag);

    FURI_LOG_D(TAG, "Timestamp");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    const char* path = request->content.storage_timestamp_request.path;
    uint32_t timestamp = 0;
    FS_Error error = storage_common_timestamp(fs_api, path, &timestamp);

    response->command_status = rpc_system_storage_get_error(error);
    response->which_content = PB_Main_empty_tag;

    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_timestamp_response_tag;
        response->content.storage_timestamp_response.timestamp = timestamp;
    }

    rpc_send_and_release(session, response);
    free(response);
    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_stat_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_stat_request_tag);

    FURI_LOG_D(TAG, "Stat");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    const char* path = request->content.storage_stat_request.path;
    FileInfo fileinfo;
    FS_Error error = storage_common_stat(fs_api, path, &fileinfo);

    response->command_status = rpc_system_storage_get_error(error);
    response->which_content = PB_Main_empty_tag;

    if(error == FSE_OK) {
        response->which_content = PB_Main_storage_stat_response_tag;
        response->content.storage_stat_response.has_file = true;
        response->content.storage_stat_response.file.type = file_info_is_dir(&fileinfo) ?
                                                                PB_Storage_File_FileType_DIR :
                                                                PB_Storage_File_FileType_FILE;
        response->content.storage_stat_response.file.size = fileinfo.size;
    }

    rpc_send_and_release(session, response);
    free(response);
    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_list_root(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    const char* hard_coded_dirs[] = {"any", "int", "ext"};

    PB_Main response = {
        .has_next = false,
        .command_id = request->command_id,
        .command_status = PB_CommandStatus_OK,
        .which_content = PB_Main_storage_list_response_tag,
    };
    furi_assert(COUNT_OF(hard_coded_dirs) < COUNT_OF(response.content.storage_list_response.file));

    for(uint32_t i = 0; i < COUNT_OF(hard_coded_dirs); ++i) {
        ++response.content.storage_list_response.file_count;
        response.content.storage_list_response.file[i].data = NULL;
        response.content.storage_list_response.file[i].size = 0;
        response.content.storage_list_response.file[i].type = PB_Storage_File_FileType_DIR;
        char* str = malloc(strlen(hard_coded_dirs[i]) + 1);
        strcpy(str, hard_coded_dirs[i]);
        response.content.storage_list_response.file[i].name = str;
    }

    rpc_send_and_release(session, &response);
}

static bool rpc_system_storage_list_filter(
    const PB_Storage_ListRequest* request,
    const FileInfo* fileinfo,
    const char* name) {
    bool result = false;

    do {
        if(!path_contains_only_ascii(name)) break;
        if(request->filter_max_size) {
            if(fileinfo->size > request->filter_max_size) break;
        }
        result = true;
    } while(false);

    return result;
}

static void rpc_system_storage_list_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_list_request_tag);

    FURI_LOG_D(TAG, "List");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    const PB_Storage_ListRequest* list_request = &request->content.storage_list_request;

    rpc_system_storage_reset_state(rpc_storage, session, true);

    if(!strcmp(list_request->path, "/")) {
        rpc_system_storage_list_root(request, context);
        return;
    }

    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(fs_api);

    PB_Main response = {
        .command_id = request->command_id,
        .has_next = false,
        .which_content = PB_Main_storage_list_response_tag,
        .command_status = PB_CommandStatus_OK,
    };
    PB_Storage_ListResponse* list = &response.content.storage_list_response;

    bool include_md5 = list_request->include_md5;
    FuriString* md5 = furi_string_alloc();
    FuriString* md5_path = furi_string_alloc();
    File* file = storage_file_alloc(fs_api);

    bool finish = false;
    int i = 0;

    if(!storage_dir_open(dir, list_request->path)) {
        response.command_status = rpc_system_storage_get_file_error(dir);
        response.which_content = PB_Main_empty_tag;
        finish = true;
    }

    while(!finish) {
        FileInfo fileinfo;
        char* name = malloc(MAX_NAME_LENGTH);
        if(storage_dir_read(dir, &fileinfo, name, MAX_NAME_LENGTH)) {
            if(rpc_system_storage_list_filter(list_request, &fileinfo, name)) {
                if(i == COUNT_OF(list->file)) {
                    list->file_count = i;
                    response.has_next = true;
                    rpc_send_and_release(session, &response);
                    i = 0;
                }
                list->file[i].type = file_info_is_dir(&fileinfo) ? PB_Storage_File_FileType_DIR :
                                                                   PB_Storage_File_FileType_FILE;
                list->file[i].size = fileinfo.size;
                list->file[i].data = NULL;
                list->file[i].name = name;

                if(include_md5 && !file_info_is_dir(&fileinfo)) {
                    furi_string_printf(md5_path, "%s/%s", list_request->path, name); //-V576

                    if(md5_string_calc_file(file, furi_string_get_cstr(md5_path), md5, NULL)) {
                        char* md5sum = list->file[i].md5sum;
                        size_t md5sum_size = sizeof(list->file[i].md5sum);
                        snprintf(md5sum, md5sum_size, "%s", furi_string_get_cstr(md5));
                    }
                }

                ++i;
            } else {
                free(name);
            }
        } else {
            list->file_count = i;
            finish = true;
            free(name);
        }
    }

    response.has_next = false;
    rpc_send_and_release(session, &response);

    furi_string_free(md5);
    furi_string_free(md5_path);
    storage_dir_close(dir);
    storage_file_free(dir);
    storage_file_free(file);

    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_read_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_read_request_tag);

    FURI_LOG_D(TAG, "Read");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    /* use same message memory to send response */
    PB_Main* response = malloc(sizeof(PB_Main));
    const char* path = request->content.storage_read_request.path;
    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(fs_api);
    bool fs_operation_success = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);

    if(fs_operation_success) {
        size_t size_left = storage_file_size(file);
        do {
            response->command_id = request->command_id;
            response->which_content = PB_Main_storage_read_response_tag;
            response->command_status = PB_CommandStatus_OK;

            size_t read_size = MIN(size_left, MAX_DATA_SIZE);
            if(read_size) {
                response->content.storage_read_response.has_file = true;
                response->content.storage_read_response.file.data =
                    malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(read_size));
                uint8_t* buffer = &response->content.storage_read_response.file.data->bytes[0];
                uint16_t* read_size_msg = &response->content.storage_read_response.file.data->size;

                *read_size_msg = storage_file_read(file, buffer, read_size);
                size_left -= *read_size_msg;
                fs_operation_success = (*read_size_msg == read_size);

                response->has_next = fs_operation_success && (size_left > 0);
            } else {
                response->content.storage_read_response.file.data =
                    malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(0));
                response->content.storage_read_response.file.data->size = 0;
                response->content.storage_read_response.has_file = true;
                response->has_next = false;
                fs_operation_success = true;
            }

            if(fs_operation_success) {
                rpc_send_and_release(session, response);
            }
        } while((size_left != 0) && fs_operation_success);
    }

    if(!fs_operation_success) {
        rpc_send_and_release_empty(
            session, request->command_id, rpc_system_storage_get_file_error(file));
    }

    free(response);
    storage_file_close(file);
    storage_file_free(file);

    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_write_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(context);
    furi_assert(request->which_content == PB_Main_storage_write_request_tag);

    FURI_LOG_D(TAG, "Write");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    bool fs_operation_success = true;

    if(!path_contains_only_ascii(request->content.storage_write_request.path)) {
        rpc_storage->current_command_id = request->command_id;
        rpc_send_and_release_empty(
            session, rpc_storage->current_command_id, PB_CommandStatus_ERROR_STORAGE_INVALID_NAME);
        rpc_system_storage_reset_state(rpc_storage, session, false);
        return;
    }

    if((request->command_id != rpc_storage->current_command_id) &&
       (rpc_storage->state == RpcStorageStateWriting)) {
        rpc_system_storage_reset_state(rpc_storage, session, true);
    }

    if(rpc_storage->state != RpcStorageStateWriting) {
        rpc_storage->api = furi_record_open(RECORD_STORAGE);
        rpc_storage->file = storage_file_alloc(rpc_storage->api);
        rpc_storage->current_command_id = request->command_id;
        rpc_storage->state = RpcStorageStateWriting;
        const char* path = request->content.storage_write_request.path;
        fs_operation_success =
            storage_file_open(rpc_storage->file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    }

    File* file = rpc_storage->file;
    bool send_response = false;

    if(fs_operation_success) {
        if(request->content.storage_write_request.has_file &&
           request->content.storage_write_request.file.data &&
           request->content.storage_write_request.file.data->size) {
            uint8_t* buffer = request->content.storage_write_request.file.data->bytes;
            size_t buffer_size = request->content.storage_write_request.file.data->size;
            size_t written_size = storage_file_write(file, buffer, buffer_size);
            fs_operation_success = (written_size == buffer_size);
        }

        send_response = !request->has_next;
    }

    PB_CommandStatus command_status = PB_CommandStatus_OK;
    if(!fs_operation_success) {
        send_response = true;
        command_status = rpc_system_storage_get_file_error(file);
        if(command_status == PB_CommandStatus_OK) {
            // Report errors not handled by underlying APIs
            command_status = PB_CommandStatus_ERROR_STORAGE_INTERNAL;
        }
    }

    if(send_response) {
        rpc_send_and_release_empty(session, rpc_storage->current_command_id, command_status);
        rpc_system_storage_reset_state(rpc_storage, session, false);
    }
}

static bool rpc_system_storage_is_dir_is_empty(Storage* fs_api, const char* path) {
    furi_assert(fs_api);
    furi_assert(path);
    FileInfo fileinfo;
    bool is_dir_is_empty = true;
    FS_Error error = storage_common_stat(fs_api, path, &fileinfo);
    if((error == FSE_OK) && file_info_is_dir(&fileinfo)) {
        File* dir = storage_file_alloc(fs_api);
        if(storage_dir_open(dir, path)) {
            char* name = malloc(MAX_NAME_LENGTH);
            while(storage_dir_read(dir, &fileinfo, name, MAX_NAME_LENGTH)) {
                if(path_contains_only_ascii(name)) {
                    is_dir_is_empty = false;
                    break;
                }
            }
            free(name);
        }
        storage_dir_close(dir);
        storage_file_free(dir);
    }

    return is_dir_is_empty;
}

static void rpc_system_storage_delete_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_delete_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Delete");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status = PB_CommandStatus_ERROR;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    char* path = request->content.storage_delete_request.path;
    if(!path) {
        status = PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    } else {
        FS_Error error_remove = storage_common_remove(fs_api, path);
        // FSE_DENIED is for empty directory, but not only for this
        // that's why we have to check it
        if((error_remove == FSE_DENIED) && !rpc_system_storage_is_dir_is_empty(fs_api, path)) {
            if(request->content.storage_delete_request.recursive) {
                bool deleted = storage_simply_remove_recursive(fs_api, path);
                status = deleted ? PB_CommandStatus_OK : PB_CommandStatus_ERROR;
            } else {
                status = PB_CommandStatus_ERROR_STORAGE_DIR_NOT_EMPTY;
            }
        } else if(error_remove == FSE_NOT_EXIST) {
            status = PB_CommandStatus_OK;
        } else {
            status = rpc_system_storage_get_error(error_remove);
        }
    }

    furi_record_close(RECORD_STORAGE);
    rpc_send_and_release_empty(session, request->command_id, status);
}

static void rpc_system_storage_mkdir_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_mkdir_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Mkdir");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    char* path = request->content.storage_mkdir_request.path;
    if(path) {
        if(path_contains_only_ascii(path)) {
            FS_Error error = storage_common_mkdir(fs_api, path);
            status = rpc_system_storage_get_error(error);
        } else {
            status = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
        }
    } else {
        status = PB_CommandStatus_ERROR_INVALID_PARAMETERS;
    }
    furi_record_close(RECORD_STORAGE);
    rpc_send_and_release_empty(session, request->command_id, status);
}

static void rpc_system_storage_md5sum_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_md5sum_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Md5sum");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, true);

    const char* filename = request->content.storage_md5sum_request.path;
    if(!filename) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    Storage* fs_api = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(fs_api);
    FuriString* md5 = furi_string_alloc();
    FS_Error file_error;

    if(md5_string_calc_file(file, filename, md5, &file_error)) {
        PB_Main response = {
            .command_id = request->command_id,
            .command_status = PB_CommandStatus_OK,
            .which_content = PB_Main_storage_md5sum_response_tag,
            .has_next = false,
        };

        char* md5sum = response.content.storage_md5sum_response.md5sum;
        size_t md5sum_size = sizeof(response.content.storage_md5sum_response.md5sum);
        snprintf(md5sum, md5sum_size, "%s", furi_string_get_cstr(md5));

        rpc_send_and_release(session, &response);
    } else {
        rpc_send_and_release_empty(
            session, request->command_id, rpc_system_storage_get_error(file_error));
    }

    furi_string_free(md5);
    storage_file_free(file);

    furi_record_close(RECORD_STORAGE);
}

static void rpc_system_storage_rename_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_rename_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "Rename");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    PB_CommandStatus status;
    rpc_system_storage_reset_state(rpc_storage, session, true);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    if(path_contains_only_ascii(request->content.storage_rename_request.new_path)) {
        FS_Error error = storage_common_rename(
            fs_api,
            request->content.storage_rename_request.old_path,
            request->content.storage_rename_request.new_path);
        status = rpc_system_storage_get_error(error);
    } else {
        status = PB_CommandStatus_ERROR_STORAGE_INVALID_NAME;
    }

    furi_record_close(RECORD_STORAGE);
    rpc_send_and_release_empty(session, request->command_id, status);
}

static void rpc_system_storage_backup_create_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_backup_create_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "BackupCreate");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    bool backup_ok =
        lfs_backup_create(fs_api, request->content.storage_backup_create_request.archive_path);

    furi_record_close(RECORD_STORAGE);

    rpc_send_and_release_empty(
        session, request->command_id, backup_ok ? PB_CommandStatus_OK : PB_CommandStatus_ERROR);
}

static void rpc_system_storage_backup_restore_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_storage_backup_restore_request_tag);
    furi_assert(context);

    FURI_LOG_D(TAG, "BackupRestore");

    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    Storage* fs_api = furi_record_open(RECORD_STORAGE);

    bool backup_ok =
        lfs_backup_unpack(fs_api, request->content.storage_backup_restore_request.archive_path);

    furi_record_close(RECORD_STORAGE);

    rpc_send_and_release_empty(
        session, request->command_id, backup_ok ? PB_CommandStatus_OK : PB_CommandStatus_ERROR);
}

void* rpc_system_storage_alloc(RpcSession* session) {
    furi_assert(session);

    RpcStorageSystem* rpc_storage = malloc(sizeof(RpcStorageSystem));
    rpc_storage->api = furi_record_open(RECORD_STORAGE);
    rpc_storage->session = session;
    rpc_storage->state = RpcStorageStateIdle;

    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = rpc_storage,
    };

    rpc_handler.message_handler = rpc_system_storage_info_process;
    rpc_add_handler(session, PB_Main_storage_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_timestamp_process;
    rpc_add_handler(session, PB_Main_storage_timestamp_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_stat_process;
    rpc_add_handler(session, PB_Main_storage_stat_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_list_process;
    rpc_add_handler(session, PB_Main_storage_list_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_read_process;
    rpc_add_handler(session, PB_Main_storage_read_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_write_process;
    rpc_add_handler(session, PB_Main_storage_write_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_delete_process;
    rpc_add_handler(session, PB_Main_storage_delete_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_mkdir_process;
    rpc_add_handler(session, PB_Main_storage_mkdir_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_md5sum_process;
    rpc_add_handler(session, PB_Main_storage_md5sum_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_rename_process;
    rpc_add_handler(session, PB_Main_storage_rename_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_backup_create_process;
    rpc_add_handler(session, PB_Main_storage_backup_create_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_storage_backup_restore_process;
    rpc_add_handler(session, PB_Main_storage_backup_restore_request_tag, &rpc_handler);

    return rpc_storage;
}

void rpc_system_storage_free(void* context) {
    furi_assert(context);
    RpcStorageSystem* rpc_storage = context;
    RpcSession* session = rpc_storage->session;
    furi_assert(session);

    rpc_system_storage_reset_state(rpc_storage, session, false);
    free(rpc_storage);
}
