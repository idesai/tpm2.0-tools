/* SPDX-License-Identifier: BSD-3-Clause */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <limits.h>
#include <tss2/tss2_esys.h>

#include "files.h"
#include "log.h"
#include "object.h"
#include "tpm2.h"
#include "tpm2_alg_util.h"
#include "tpm2_auth_util.h"
#include "tpm2_options.h"
#include "tpm2_session.h"
#include "tpm2_tool.h"
#include "tpm2_util.h"

typedef struct tpm_hmac_ctx tpm_hmac_ctx;
struct tpm_hmac_ctx {
    struct {
        char *ctx_path;
        char *auth_str;
        tpm2_loaded_object object;
    } hmac_key;

    FILE *input;
    char *hmac_output_file_path;
};

static tpm_hmac_ctx ctx;

static tool_rc tpm_hmac_file(ESYS_CONTEXT *ectx, TPM2B_DIGEST **result) {

    unsigned long file_size = 0;
    FILE *input = ctx.input;

    tool_rc rc;
    /* Suppress error reporting with NULL path */
    bool res = files_get_file_size(input, &file_size, NULL);

    /* If we can get the file size and its less than 1024, just do it in one hash invocation */
    if (res && file_size <= TPM2_MAX_DIGEST_BUFFER) {

        TPM2B_MAX_BUFFER buffer = { .size = file_size };

        res = files_read_bytes(ctx.input, buffer.buffer, buffer.size);
        if (!res) {
            LOG_ERR("Error reading input file!");
            return tool_rc_general_error;
        }
        /*
         * hash algorithm specified in the key's scheme is used as the
         * hash algorithm for the HMAC
         */
        return tpm2_hmac(ectx, &ctx.hmac_key.object, &buffer, result);
    }

    ESYS_TR sequence_handle;
    /*
     * Size is either unknown because the FILE * is a fifo, or it's too big
     * to do in a single hash call. Based on the size figure out the chunks
     * to loop over, if possible. This way we can call Complete with data.
     */
    rc = tpm2_hmac_start(ectx, &ctx.hmac_key.object, &sequence_handle);
    if (rc != tool_rc_success) {
        return tool_rc_general_error;
    }

    /* If we know the file size, we decrement the amount read and terminate the
     * loop when 1 block is left, else we go till feof.
     */
    size_t left = file_size;
    bool use_left = !!res;

    TPM2B_MAX_BUFFER data;

    bool done = false;
    while (!done) {

        size_t bytes_read = fread(data.buffer, 1,
                BUFFER_SIZE(typeof(data), buffer), input);
        if (ferror(input)) {
            LOG_ERR("Error reading from input file");
            return tool_rc_general_error;
        }

        data.size = bytes_read;

        /* if data was read, update the sequence */
        rc = tpm2_hmac_sequenceupdate(ectx, sequence_handle,
            &ctx.hmac_key.object, &data);
        if (rc != tool_rc_success) {
            return tool_rc_general_error;
        }

        if (use_left) {
            left -= bytes_read;
            if (left <= TPM2_MAX_DIGEST_BUFFER) {
                done = true;
                continue;
            }
        } else if (feof(input)) {
            done = true;
        }
    } /* end file read/hash update loop */

    if (use_left) {
        data.size = left;
        bool res = files_read_bytes(input, data.buffer, left);
        if (!res) {
            LOG_ERR("Error reading from input file.");
            return tool_rc_general_error;
        }
    } else {
        data.size = 0;
    }

    rc = tpm2_hmac_sequencecomplete(ectx, sequence_handle,
        &ctx.hmac_key.object, &data, result);
        if (rc != tool_rc_success) {
            return tool_rc_general_error;
        }

    return tool_rc_success;
}


static tool_rc do_hmac_and_output(ESYS_CONTEXT *ectx) {

    TPM2B_DIGEST *hmac_out = NULL;

    tool_rc rc = tpm_hmac_file(ectx, &hmac_out);
    if (rc != tool_rc_success) {
        goto out;
    }

    assert(hmac_out);

    if (hmac_out->size) {
        UINT16 i;
        for (i = 0; i < hmac_out->size; i++) {
            tpm2_tool_output("%02x", hmac_out->buffer[i]);
        }
        tpm2_tool_output("\n");
    }

    if (ctx.hmac_output_file_path) {
        bool result = files_save_bytes_to_file(ctx.hmac_output_file_path, hmac_out->buffer,
            hmac_out->size);
        if (!result) {
            rc = tool_rc_general_error;
        }
    }

out:
    free(hmac_out);

    return rc;
}

static bool on_option(char key, char *value) {

    switch (key) {
    case 'C':
        ctx.hmac_key.ctx_path = value;
        break;
    case 'P':
        ctx.hmac_key.auth_str = value;
        break;
    case 'o':
        ctx.hmac_output_file_path = value;
        break;
    }

    return true;
}

static bool on_args(int argc, char **argv) {

    if (argc > 1) {
        LOG_ERR("Expected 1 hmac input file, got: %d", argc);
        return false;
    }

    ctx.input = fopen(argv[0], "rb");
    if (!ctx.input) {
        LOG_ERR("Error opening file \"%s\", error: %s", argv[0],
                strerror(errno));
        return false;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
        { "key-context",          required_argument, NULL, 'C' },
        { "auth-key",             required_argument, NULL, 'P' },
        { "out-file",             required_argument, NULL, 'o' },
    };

    ctx.input = stdin;

    *opts = tpm2_options_new("C:P:o:", ARRAY_LEN(topts), topts, on_option,
                             on_args, 0);

    return *opts != NULL;
}

tool_rc tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {

    UNUSED(flags);

    /*
     * Option C must be specified.
     */
    if (!ctx.hmac_key.ctx_path) {
        LOG_ERR("Must specify options C.");
        return tool_rc_option_error;
    }

    tool_rc rc = tpm2_util_object_load_auth(ectx, ctx.hmac_key.ctx_path,
        ctx.hmac_key.auth_str, &ctx.hmac_key.object, false);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid key handle authorization, got\"%s\"",
            ctx.hmac_key.auth_str);
        return rc;
    }

    return do_hmac_and_output(ectx);
}

tool_rc tpm2_tool_onstop(ESYS_CONTEXT *ectx) {
    UNUSED(ectx);

    if (ctx.input && ctx.input != stdin) {
        fclose(ctx.input);
    }

    return tpm2_session_close(&ctx.hmac_key.object.session);
}
