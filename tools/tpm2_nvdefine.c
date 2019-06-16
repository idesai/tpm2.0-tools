/* SPDX-License-Identifier: BSD-3-Clause */

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
#include "tpm2_attr_util.h"
#include "tpm2_auth_util.h"
#include "tpm2_hierarchy.h"
#include "tpm2_options.h"
#include "tpm2_session.h"
#include "tpm2_tool.h"
#include "tpm2_util.h"

typedef struct tpm_nvdefine_ctx tpm_nvdefine_ctx;
struct tpm_nvdefine_ctx {
    struct {
        const char *ctx_path;
        const char *auth_str;
        tpm2_loaded_object object;
    } auth_hierarchy;

    UINT32 nvIndex;
    UINT16 size;
    TPMA_NV nvAttribute;
    TPM2B_AUTH nvAuth;

    char *policy_file;
    char *index_auth_str;
};

static tpm_nvdefine_ctx ctx = {
    .auth_hierarchy = {
        .ctx_path = "o",
    },
    .nvAuth = TPM2B_EMPTY_INIT,
    .size = TPM2_MAX_NV_BUFFER_SIZE,
};

static tool_rc nv_space_define(ESYS_CONTEXT *ectx) {

    TPM2B_NV_PUBLIC public_info = TPM2B_EMPTY_INIT;

    public_info.size = sizeof(TPMI_RH_NV_INDEX) + sizeof(TPMI_ALG_HASH)
            + sizeof(TPMA_NV) + sizeof(UINT16) + sizeof(UINT16);
    public_info.nvPublic.nvIndex = ctx.nvIndex;
    public_info.nvPublic.nameAlg = TPM2_ALG_SHA256;

    // Now set the attributes.
    public_info.nvPublic.attributes = ctx.nvAttribute;

    if (!ctx.size) {
        LOG_WARN("Defining an index with size 0");
    }

    if (ctx.policy_file) {
        public_info.nvPublic.authPolicy.size  = BUFFER_SIZE(TPM2B_DIGEST, buffer);
        if(!files_load_bytes_from_path(ctx.policy_file, public_info.nvPublic.authPolicy.buffer, &public_info.nvPublic.authPolicy.size )) {
            return tool_rc_general_error;
        }
    }

    public_info.nvPublic.dataSize = ctx.size;

    tool_rc rc = tpm2_nv_definespace(ectx, &ctx.auth_hierarchy.object, &ctx.nvAuth,
        &public_info);
    if (rc != tool_rc_success) {
        LOG_INFO("Success to define NV area at index 0x%x.", ctx.nvIndex);
        return rc;
    }

    return tool_rc_success;
}

static bool on_option(char key, char *value) {

    bool result;

    switch (key) {
    case 'x':
        result = tpm2_util_string_to_uint32(value, &ctx.nvIndex);
        if (!result) {
            LOG_ERR("Could not convert NV index to number, got: \"%s\"",
                    value);
            return false;
        }

        if (ctx.nvIndex == 0) {
                LOG_ERR("NV Index cannot be 0");
                return false;
        }
        break;
        case 'a':
            ctx.auth_hierarchy.ctx_path = value;
        break;
        case 'P':
            ctx.auth_hierarchy.auth_str = value;
        break;
        case 's':
            result = tpm2_util_string_to_uint16(value, &ctx.size);
            if (!result) {
                LOG_ERR("Could not convert size to number, got: \"%s\"",
                        value);
                return false;
            }
            break;
        case 'b':
            result = tpm2_util_string_to_uint32(value, &ctx.nvAttribute);
            if (!result) {
                result = tpm2_attr_util_nv_strtoattr(value, &ctx.nvAttribute);
                if (!result) {
                    LOG_ERR("Could not convert NV attribute to number or keyword, got: \"%s\"",
                            value);
                    return false;
                }
            }
            break;
        case 'p':
            ctx.index_auth_str = value;
            break;
        case 'L':
            ctx.policy_file = value;
            break;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
        { "index",                  required_argument,  NULL,   'x' },
        { "hierarchy",              required_argument,  NULL,   'a' },
        { "size",                   required_argument,  NULL,   's' },
        { "attributes",             required_argument,  NULL,   'b' },
        { "auth-hierarchy",         required_argument,  NULL,   'P' },
        { "auth-index",             required_argument,  NULL,   'p' },
        { "policy-file",            required_argument,  NULL,   'L' },
    };

    *opts = tpm2_options_new("x:a:s:b:P:p:L:", ARRAY_LEN(topts), topts,
                             on_option, NULL, 0);

    return *opts != NULL;
}

tool_rc tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {

    UNUSED(flags);

    tool_rc rc = tpm2_util_object_load_auth(ectx, ctx.auth_hierarchy.ctx_path,
            ctx.auth_hierarchy.auth_str, &ctx.auth_hierarchy.object, false);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid authorization, got\"%s\"", ctx.auth_hierarchy.auth_str);
        return rc;
    }


    tpm2_session *tmp;
    rc = tpm2_auth_util_from_optarg(NULL, ctx.index_auth_str,
            &tmp, true);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid index authorization, got\"%s\"", ctx.index_auth_str);
        return rc;
    }

    const TPM2B_AUTH *auth = tpm2_session_get_auth_value(tmp);
    ctx.nvAuth = *auth;

    tpm2_session_close(&tmp);



    return nv_space_define(ectx);
}

tool_rc tpm2_tool_onstop(ESYS_CONTEXT *ectx) {
    UNUSED(ectx);
    return tpm2_session_close(&ctx.auth_hierarchy.object.session);
}
