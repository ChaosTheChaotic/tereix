#include "lsp.h"

void lsp_send_response(yyjson_mut_val *result_val, yyjson_mut_val *id_val) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    if (id_val) yyjson_mut_obj_add_val(doc, root, "id", id_val);
    if (result_val) yyjson_mut_obj_add_val(doc, root, "result", result_val);

    const char *json_str = yyjson_mut_write(doc, 0, NULL);
    if (json_str) {
        fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str), json_str);
        fflush(stdout);
        free((void *)json_str);
    }
    yyjson_mut_doc_free(doc);
}

void handle_initialize(yyjson_val *params, yyjson_mut_val *id) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_val *capabilities = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_int(doc, capabilities, "textDocumentSync", 1); 
    
    // TODO: Add more capabilities here later (hoverProvider, definitionProvider, etc.)

    yyjson_mut_obj_add_val(doc, result, "capabilities", capabilities);
    lsp_send_response(result, id);
    yyjson_mut_doc_free(doc);
}

void start_lsp_server() {
    // Disable stdout buffering so messages go straight to the editor
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        int content_length = 0;
        char buffer[256];

        // Read headers until we hit the empty line "\r\n"
        while (fgets(buffer, sizeof(buffer), stdin)) {
            if (strncmp(buffer, "Content-Length: ", 16) == 0) {
                content_length = atoi(buffer + 16);
            }
            if (strcmp(buffer, "\r\n") == 0) {
                break;
            }
        }

        if (content_length <= 0) continue;

        char *payload = malloc(content_length + 1);
        fread(payload, 1, content_length, stdin);
        payload[content_length] = '\0';

        yyjson_doc *doc = yyjson_read(payload, strlen(payload), 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            const char *method = yyjson_get_str(yyjson_obj_get(root, "method"));
            
            yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *id = yyjson_val_mut_copy(mut_doc, yyjson_obj_get(root, "id"));

            if (method) {
                if (strcmp(method, "initialize") == 0) {
                    handle_initialize(yyjson_obj_get(root, "params"), id);
                }
                // Add more routing here (e.g., textDocument/didOpen, textDocument/didChange)
            }
            
            yyjson_mut_doc_free(mut_doc);
            yyjson_doc_free(doc);
        }
        free(payload);
    }
}
