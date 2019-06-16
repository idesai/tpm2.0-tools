/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <limits.h>
#include <ctype.h>

#include <tss2/tss2_esys.h>

#include "files.h"
#include "log.h"
#include "object.h"
#include "tpm2_auth_util.h"
#include "tpm2_error.h"
#include "tpm2_options.h"
#include "tpm2_util.h"
#include "tpm2_session.h"
#include "tpm2_tool.h"

typedef struct tpm_activatecred_ctx tpm_activatecred_ctx;
struct tpm_activatecred_ctx {
    struct {
        UINT8 i : 1;
        UINT8 o : 1;
    } flags;

    struct {
        /*
         * Typically AK pass
         */
        char *credentialed_key_auth_str;
        tpm2_session *session;
        /*
         * Typically EK
         */
        const char *credential_key_arg;
    } key;

    struct {
        /*
         * Typically EK pass
         */
        char *credential_key_auth_str;
        tpm2_session *session;
    } credential_key;

    TPM2B_ID_OBJECT credentialBlob;
    TPM2B_ENCRYPTED_SECRET secret;

    const char *output_file;
    /*
     * Typically AK
     */
    const char *credentialed_key_arg;
    tpm2_loaded_object credentialed_key_obj;
    tpm2_loaded_object credential_key_obj;
};

static tpm_activatecred_ctx ctx;

static bool read_cert_secret(const char *path, TPM2B_ID_OBJECT *cred,
        TPM2B_ENCRYPTED_SECRET *secret) {

    bool result = false;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("Could not open file \"%s\" error: \"%s\"", path,
                strerror(errno));
        return false;
    }

    uint32_t version;
    result = files_read_header(fp, &version);
    if (!result) {
        LOG_ERR("Could not read version header");
        goto out;
    }

    if (version != 1) {
        LOG_ERR("Unknown credential format, got %"PRIu32" expected 1",
                version);
        goto out;
    }

    result = files_read_16(fp, &cred->size);
    if (!result) {
        LOG_ERR("Could not read credential size");
        goto out;
    }

    result = files_read_bytes(fp, cred->credential, cred->size);
    if (!result) {
        LOG_ERR("Could not read credential data");
        goto out;
    }

    result = files_read_16(fp, &secret->size);
    if (!result) {
        LOG_ERR("Could not read secret size");
        goto out;
    }

    result = files_read_bytes(fp, secret->secret, secret->size);
    if (!result) {
        LOG_ERR("Could not write secret data");
        goto out;
    }

    result = true;

out:
    fclose(fp);
    return result;
}

static bool output_and_save(TPM2B_DIGEST *digest, const char *path) {

    tpm2_tool_output("certinfodata:");

    unsigned k;
    for (k = 0; k < digest->size; k++) {
        tpm2_tool_output("%.2x", digest->buffer[k]);
    }
    tpm2_tool_output("\n");

    return files_save_bytes_to_file(path, digest->buffer, digest->size);
}

static tool_rc activate_credential_and_output(ESYS_CONTEXT *ectx) {

    tool_rc rc = tool_rc_general_error;

    TPM2B_DIGEST *certInfoData;

    tpm2_session_data *d = tpm2_session_data_new(TPM2_SE_POLICY);
    if (!d) {
        LOG_ERR("oom");
        return false;
    }

    tpm2_session *session = NULL;
    tool_rc tmp_rc = tpm2_session_open(ectx, d, &session);
    if (tmp_rc != tool_rc_success) {
        return tmp_rc;
    }

    // Set session up
    ESYS_TR sess_handle = tpm2_session_get_handle(session);

    ESYS_TR credential_key_shandle = ESYS_TR_NONE;
    tmp_rc = tpm2_auth_util_get_shandle(ectx, ESYS_TR_RH_ENDORSEMENT,
                                ctx.credential_key.session, &credential_key_shandle);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
        goto out_session;
    }

    TSS2_RC rval = Esys_PolicySecret(ectx, ESYS_TR_RH_ENDORSEMENT, sess_handle,
                    credential_key_shandle, ESYS_TR_NONE, ESYS_TR_NONE,
                    NULL, NULL, NULL, 0, NULL, NULL);
    if (rval != TPM2_RC_SUCCESS) {
        LOG_PERR(Esys_PolicySecret, rval);
        rc = tool_rc_from_tpm(rval);
        goto out_session;
    }

    ESYS_TR key_shandle = ESYS_TR_NONE;
    tmp_rc = tpm2_auth_util_get_shandle(ectx,
                            ctx.credentialed_key_obj.tr_handle,
                            ctx.key.session, &key_shandle);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
        goto out_session;
    }

    rval = Esys_ActivateCredential(ectx, ctx.credentialed_key_obj.tr_handle,
            ctx.credential_key_obj.tr_handle,
            key_shandle, sess_handle, ESYS_TR_NONE,
            &ctx.credentialBlob, &ctx.secret, &certInfoData);
    if (rval != TPM2_RC_SUCCESS) {
        LOG_PERR(Esys_ActivateCredential, rval);
        rc = tool_rc_from_tpm(rval);
        goto out_all;
    }

    bool result = output_and_save(certInfoData, ctx.output_file);
    if (!result) {
        goto out_all;
    }

    rc = tool_rc_success;

out_all:
    free(certInfoData);
out_session:
    tpm2_session_close(&session);

    return rc;
}

static bool on_option(char key, char *value) {

    bool result;

    switch (key) {
    case 'c':
        ctx.credentialed_key_arg = value;
        break;
    case 'P':
        ctx.key.credentialed_key_auth_str = value;
        break;
    case 'C':
        ctx.key.credential_key_arg = value;
        break;
    case 'E':
        ctx.credential_key.credential_key_auth_str = value;
        break;
    case 'i':
        /* logs errors */
        result = read_cert_secret(value, &ctx.credentialBlob,
                &ctx.secret);
        if (!result) {
            return false;
        }
        ctx.flags.i = 1;
        break;
    case 'o':
        ctx.output_file = value;
        ctx.flags.o = 1;
        break;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    static const struct option topts[] = {
         {"credentialedkey-context", required_argument, NULL, 'c'},
         {"credentialkey-context",      required_argument, NULL, 'C'},
         {"credentialedkey-auth",    required_argument, NULL, 'P'},
         {"credentialkey-auth",         required_argument, NULL, 'E'},
         {"credential-secret",      required_argument, NULL, 'i'},
         {"certinfo-data",          required_argument, NULL, 'o'},
    };

    *opts = tpm2_options_new("c:C:P:E:i:o:", ARRAY_LEN(topts), topts,
                             on_option, NULL, 0);

    return *opts != NULL;
}

tool_rc tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {

    /* opts is unused, avoid compiler warning */
    UNUSED(flags);

    if ((!ctx.credentialed_key_arg)
            && (!ctx.key.credential_key_arg)
            && !ctx.flags.i && !ctx.flags.o) {
        LOG_ERR("Expected options c and C and i and o.");
        return tool_rc_option_error;
    }

    tool_rc rc = tpm2_util_object_load(ectx, ctx.credentialed_key_arg,
                                &ctx.credentialed_key_obj);
    if (rc != tool_rc_success) {
        return rc;
    }

    rc = tpm2_util_object_load(ectx, ctx.key.credential_key_arg,
                &ctx.credential_key_obj);
    if (rc != tool_rc_success) {
        return rc;
    }

    rc = tpm2_auth_util_from_optarg(ectx, ctx.key.credentialed_key_auth_str,
            &ctx.key.session, false);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid activateHandle authorization, got\"%s\"", ctx.key.credentialed_key_auth_str);
        return rc;
    }

    rc = tpm2_auth_util_from_optarg(NULL, ctx.credential_key.credential_key_auth_str,
            &ctx.credential_key.session, true);
    if (rc != tool_rc_success) {
        LOG_ERR("Invalid keyHandle authorization, got\"%s\"", ctx.credential_key.credential_key_auth_str);
        return rc;
    }

    return activate_credential_and_output(ectx);
}

tool_rc tpm2_tool_onstop(ESYS_CONTEXT *ectx) {
    UNUSED(ectx);

    tool_rc rc = tool_rc_success;

    tool_rc tmp_rc = tpm2_session_close(&ctx.key.session);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
    }

    tmp_rc = tpm2_session_close(&ctx.credential_key.session);
    if (tmp_rc != tool_rc_success) {
        rc = tmp_rc;
    }

    return rc;
}
