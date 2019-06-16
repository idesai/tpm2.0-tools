/* SPDX-License-Identifier: BSD-3-Clause */

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <tss2/tss2_esys.h>

#include "files.h"
#include "log.h"
#include "object.h"
#include "tpm2.h"
#include "tpm2_alg_util.h"
#include "tpm2_auth_util.h"
#include "tpm2_convert.h"
#include "tpm2_options.h"
#include "tpm2_session.h"
#include "tpm2_tool.h"
#include "tpm2_util.h"

typedef struct tpm_certify_ctx tpm_certify_ctx;
struct tpm_certify_ctx {
    struct {
        const char *ctx_path;
        const char *auth_str;
        tpm2_loaded_object object;
    } object;

    struct {
        const char *ctx_path;
        const char *auth_str;
        tpm2_loaded_object object;
    } key;

    struct {
        char *attest;
        char *sig;
    } file_path;

    struct {
        UINT16 g : 1;
        UINT16 o : 1;
        UINT16 s : 1;
        UINT16 f : 1;
    } flags;

    TPMI_ALG_HASH halg;
    tpm2_convert_sig_fmt sig_fmt;
};

static tpm_certify_ctx ctx = {
    .sig_fmt = signature_format_tss,
};

static tool_rc get_key_type(ESYS_CONTEXT *ectx, ESYS_TR object_handle,
                            TPMI_ALG_PUBLIC *type) {

    TPM2B_PUBLIC *out_public = NULL;
    tool_rc rc = tpm2_readpublic(ectx, object_handle,
                ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                &out_public, NULL, NULL);
    if (rc != tool_rc_success) {
        return rc;
    }

    *type = out_public->publicArea.type;

    free(out_public);

    return tool_rc_success;
}

static tool_rc set_scheme(ESYS_CONTEXT *ectx, ESYS_TR key_handle,
        TPMI_ALG_HASH halg, TPMT_SIG_SCHEME *scheme) {

    TPM2_ALG_ID type;
    tool_rc rc = get_key_type(ectx, key_handle, &type);
    if (rc != tool_rc_success) {
        return rc;
    }

    switch (type) {
    case TPM2_ALG_RSA :
        scheme->scheme = TPM2_ALG_RSASSA;
        scheme->details.rsassa.hashAlg = halg;
        break;
    case TPM2_ALG_KEYEDHASH :
        scheme->scheme = TPM2_ALG_HMAC;
        scheme->details.hmac.hashAlg = halg;
        break;
    case TPM2_ALG_ECC :
        scheme->scheme = TPM2_ALG_ECDSA;
        scheme->details.ecdsa.hashAlg = halg;
        break;
    case TPM2_ALG_SYMCIPHER :
    default:
        LOG_ERR("Unknown key type, got: 0x%x", type);
        return false;
    }

    return tool_rc_success;
}

static tool_rc certify_and_save_data(ESYS_CONTEXT *ectx) {

    TPM2B_DATA qualifying_data = {
        .size = 4,
        .buffer = { 0x00, 0xff, 0x55,0xaa }
    };

    TPMT_SIG_SCHEME scheme;
    tool_rc rc = set_scheme(ectx, ctx.key.object.tr_handle, ctx.halg,
                    &scheme);
    if (rc != tool_rc_success) {
        LOG_ERR("No suitable signing scheme!");
        return rc;
    }

    TPM2B_ATTEST *certify_info;
    TPMT_SIGNATURE *signature;

    rc = tpm2_certify(
            ectx,
            &ctx.object.object,
            &ctx.key.object,
            &qualifying_data,
            &scheme,
            &certify_info,
            &signature);
    if (rc != tool_rc_success) {
        return rc;
    }
    /* serialization is safe here, since it's just a byte array */
    bool result = files_save_bytes_to_file(ctx.file_path.attest,
            certify_info->attestationData, certify_info->size);
    if (!result) {
        goto out;
    }

    result = tpm2_convert_sig_save(signature, ctx.sig_fmt, ctx.file_path.sig);
    if (!result) {
        goto out;
    }

    rc = tool_rc_success;

out:
    free(certify_info);
    free(signature);

    return rc;
}

static bool on_option(char key, char *value) {

    switch (key) {
    case 'C':
        ctx.object.ctx_path = value;
        break;
    case 'c':
        ctx.key.ctx_path = value;
        break;
    case 'P':
        ctx.object.auth_str = value;
        break;
    case 'p':
        ctx.key.auth_str = value;
        break;
    case 'g':
        ctx.halg = tpm2_alg_util_from_optarg(value, tpm2_alg_util_flags_hash);
        if (ctx.halg == TPM2_ALG_ERROR) {
            LOG_ERR("Could not format algorithm to number, got: \"%s\"", value);
            return false;
        }
        ctx.flags.g = 1;
        break;
    case 'o':
        ctx.file_path.attest = value;
        ctx.flags.o = 1;
        break;
    case 's':
        ctx.file_path.sig = value;
        ctx.flags.s = 1;
        break;
    case 'f':
        ctx.flags.f = 1;
        ctx.sig_fmt = tpm2_convert_sig_fmt_from_optarg(value);

        if (ctx.sig_fmt == signature_format_err) {
            return false;
        }
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
      { "auth-object",      required_argument, NULL, 'P' },
      { "auth-key",         required_argument, NULL, 'p' },
      { "halg",             required_argument, NULL, 'g' },
      { "out-attest-file",  required_argument, NULL, 'o' },
      { "sig-file",         required_argument, NULL, 's' },
      { "obj-context",      required_argument, NULL, 'C' },
      { "key-context",      required_argument, NULL, 'c' },
      { "format",           required_argument, NULL, 'f' },
    };

    *opts = tpm2_options_new("P:p:g:o:s:c:C:f:", ARRAY_LEN(topts), topts,
                             on_option, NULL, 0);

    return *opts != NULL;
}

tool_rc tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {
    UNUSED(flags);

    if ((!ctx.object.ctx_path)
        && (!ctx.key.ctx_path)
        && (ctx.flags.g) && (ctx.flags.o)
        && (ctx.flags.s)) {
        return tool_rc_option_error;
    }

    /* Load input files */
    tool_rc rc = tpm2_util_object_load_auth(ectx, ctx.object.ctx_path,
        ctx.object.auth_str, &ctx.object.object, false);
    if (rc != tool_rc_success) {
        return rc;
    }

    rc = tpm2_util_object_load_auth(ectx, ctx.key.ctx_path,
        ctx.key.auth_str, &ctx.key.object, false);
    if (rc != tool_rc_success) {
        return rc;
    }

    return certify_and_save_data(ectx);
}

tool_rc tpm2_tool_onstop(ESYS_CONTEXT *ectx) {
    UNUSED(ectx);

    tool_rc rc = tool_rc_success;

    tool_rc tmp_rc = tpm2_session_close(&ctx.key.object.session);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
    }

    tmp_rc = tpm2_session_close(&ctx.object.object.session);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
    }

    return rc;
}
