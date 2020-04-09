/* Copyright (C) 2018-2020 Intel Labs
   This file is part of Graphene Library OS.
   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.
   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*!
 * \file
 *
 * This file contains the implementation of verification callbacks for TLS libraries. The callbacks
 * verify the correctness of a self-signed RA-TLS certificate with an SGX quote embedded in it. The
 * callbacks access Intel Attestation Service (IAS) for EPID-based attestation as part of the
 * verification process. A callback ra_tls_verify_callback() can be used directly in mbedTLS, and
 * a more generic version ra_tls_verify_callback_der() should be used for other TLS libraries.
 *
 * This file is part of the RA-TLS verification library which is typically linked into client
 * applications.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "attestation.h"
#include "ias.h"
#include "ra_tls.h"
#include "sgx_arch.h"
#include "sgx_attest.h"
#include "util.h"

/** Default base URL for IAS API endpoints. Remove "/dev" for production environment. */
#define IAS_URL_BASE "https://api.trustedservices.intel.com/sgx/dev"

/** Default URL for IAS "verify attestation evidence" API endpoint. */
#define IAS_URL_REPORT IAS_URL_BASE "/attestation/v3/report"

/** Default URL for IAS "Retrieve SigRL" API endpoint. EPID group id is added at the end. */
#define IAS_URL_SIGRL IAS_URL_BASE "/attestation/v3/sigrl"

static char* g_api_key         = NULL;
static char* g_report_url      = NULL;
static char* g_sigrl_url       = NULL;

static int init_api_key(void) {
    if (g_api_key) {
        /* already initialized */
        return 0;
    }

    char* envvar_key = getenv(RA_TLS_EPID_API_KEY);
    if (!envvar_key)
        return -1;

    size_t envvar_key_size = strlen(envvar_key) + 1;
    g_api_key = malloc(envvar_key_size);
    if (!g_api_key)
        return -1;

    memcpy(g_api_key, envvar_key, envvar_key_size);
    return 0;
}

static int init_report_url(void) {
    if (g_report_url) {
        /* already initialized */
        return 0;
    }

    char* envvar_url = getenv(RA_TLS_REPORT_URL);
    if (!envvar_url) {
        /* use default report URL */
        g_report_url = IAS_URL_REPORT;
        return 0;
    }

    size_t envvar_url_size = strlen(envvar_url) + 1;
    g_report_url = malloc(envvar_url_size);
    if (!g_report_url)
        return -1;

    memcpy(g_report_url, envvar_url, envvar_url_size);
    return 0;
}

static int init_sigrl_url(void) {
    if (g_sigrl_url) {
        /* already initialized */
        return 0;
    }

    char* envvar_url = getenv(RA_TLS_SIGRL_URL);
    if (!envvar_url) {
        /* use default signature revocation list URL */
        g_sigrl_url = IAS_URL_SIGRL;
        return 0;
    }

    size_t envvar_url_size = strlen(envvar_url) + 1;
    g_sigrl_url = malloc(envvar_url_size);
    if (!g_sigrl_url)
        return -1;

    memcpy(g_sigrl_url, envvar_url, envvar_url_size);
    return 0;
}

static int generate_nonce(char* buf, size_t size) {
    FILE* f = fopen("/dev/urandom", "r");
    if (!f)
        return -1;

    size_t nmemb = fread(buf, size, 1, f);
    if (nmemb != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static int getenv_allow_outdated_tcb(bool* allow_outdated_tcb) {
    *allow_outdated_tcb = false;

    char* str = getenv(RA_TLS_ALLOW_OUTDATED_TCB);
    if (str && *str != '0' && *str != 'f' && *str != 'F') {
        /* any value that is not "0", "false", or "FALSE" is considered true */
        *allow_outdated_tcb = true;
    }
    return 0;
}

static int getenv_enclave_measurements(const char** mrsigner, const char** mrenclave,
                                       const char** isv_prod_id, const char** isv_svn) {
    /* any of the below variables may be NULL (and then not used in validation) */
    *mrsigner    = getenv(RA_TLS_MRSIGNER);
    *mrenclave   = getenv(RA_TLS_MRENCLAVE);
    *isv_prod_id = getenv(RA_TLS_ISV_PROD_ID);
    *isv_svn     = getenv(RA_TLS_ISV_SVN);
    return 0;
}

static int getenv_ias_pub_key_pem(const char** ias_pub_key_pem) {
    /* may be NULL (and then a hard-coded public key of IAS is used) */
    *ias_pub_key_pem = getenv(RA_TLS_IAS_PUB_KEY_PEM);
    return 0;
}

/*! searches for specific \p oid among \p exts and returns pointer to its value in \p val */
static int find_oid(const uint8_t* exts, size_t exts_len, const uint8_t* oid, size_t oid_len,
                    uint8_t** val, size_t* len) {
    /* TODO: searching with memmem is not robust (what if some extension contains exactly these
     *       chars?), but mbedTLS has nothing generic enough for our purposes */
    uint8_t* p = memmem(exts, exts_len, oid, oid_len);
    if (!p)
        return -1;

    /* move pointer past OID string and to the OID value */
    p += oid_len;

    if (*p == 0x01) {
        /* some TLS libs generate a BOOLEAN for the criticality of the extension before the
         * extension value itself; check its value and skip it */
        p++;
        if (*p++ != 0x01) {
            /* BOOLEAN length must be 0x01 */
            return -1;
        }
        if (*p++ != 0x00) {
            /* BOOLEAN value must be 0x00 (non-critical extension) */
            return -1;
        }
    }

    /* now comes the octet string */
    if (*p++ != 0x04) {
        /* tag for octet string must be 0x04 */
        return -1;
    }
    if (*p++ != 0x82) {
        /* length of octet string must be 0x82 (encoded in two bytes) */
        return -1;
    }

    *len   = *p++;
    *len <<= 8;
    *len  += *p++;

    *val = p;
    return 0;
}

/*! calculate sha256 over public key from \p crt and copy it into \p sha */
static int sha256_over_crt_pk(mbedtls_x509_crt *crt, uint8_t* sha) {
    uint8_t pk_der[PUB_KEY_SIZE_MAX] = {0};

    /* below function writes data at the end of the buffer */
    int pk_der_size_byte = mbedtls_pk_write_pubkey_der(&crt->pk, pk_der, PUB_KEY_SIZE_MAX);
    if (pk_der_size_byte != RSA_PUB_3072_KEY_DER_LEN)
        return -1;

    /* move the data to the beginning of the buffer, to avoid pointer arithmetic later */
    memmove(pk_der, pk_der + PUB_KEY_SIZE_MAX - pk_der_size_byte, pk_der_size_byte);

    int ret = mbedtls_sha256_ret(pk_der, pk_der_size_byte, sha, /*is224=*/0);
    if (ret)
        return -1;

    return 0;
}

/*! compares if report_data from \quote corresponds to sha256 of public key in \p crt */
static int cmp_crt_pk_against_quote_report_data(mbedtls_x509_crt *crt, sgx_quote_t* quote) {
    int ret;

    uint8_t sha[SHA256_DIGEST_SIZE];
    ret = sha256_over_crt_pk(crt, sha);
    if (ret < 0)
        return -1;

    ret = memcmp(quote->report_body.report_data.d, sha, SHA256_DIGEST_SIZE);
    if (ret)
        return -1;

    return 0;
}

int ra_tls_verify_callback(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    (void)data;

    int ret;
    struct ias_context_t* ias = NULL;

    char* report_path         = NULL;
    char* sig_path            = NULL;
    char* cert_path           = NULL;
    char* advisory_path       = NULL;

    uint8_t* report_blob      = NULL;
    uint8_t* sig_blob         = NULL;

    if (depth != 0) {
        /* only interested in peer cert (at the end of cert chain): it contains RA-TLS info */
        return 0;
    }

    if (flags && *flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        /* mbedTLS sets this to signal that the cert is not correctly signed by a trusted CA;
         * since RA-TLS uses self-signed certs, we don't care what mbedTLS thinks */
        *flags = 0;
    }

    if (init_api_key() < 0 || init_report_url() < 0 || init_sigrl_url() < 0) {
        ret = -1;
        goto out;
    }

    /* extract SGX quote from "quote" OID extension from crt */
    sgx_quote_t* quote;
    size_t quote_size;
    ret = find_oid(crt->v3_ext.p, crt->v3_ext.len, quote_oid, quote_oid_len, (uint8_t**)&quote,
                   &quote_size);
    if (ret < 0 || quote_size < sizeof(*quote)) {
        ret = -1;
        goto out;
    }

    /* compare public key's hash from cert against quote's report_data */
    ret = cmp_crt_pk_against_quote_report_data(crt, quote);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    /* initialize the IAS context, send the quote to the IAS, receive IAS attestation report,
     * and save its components (report, signature, certs, etc.) in temporary files */
    ias = ias_init(g_api_key, g_report_url, g_sigrl_url);
    if (!ias) {
        ret = -1;
        goto out;
    }

    char nonce[IAS_REQUEST_NONCE_SIZE];
    ret = generate_nonce(nonce, sizeof(nonce));
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    report_path = tempnam(P_tmpdir, "ra_tls_verify_report_");
    if (!report_path) {
        ret = -1;
        goto out;
    }

    sig_path = tempnam(P_tmpdir, "ra_tls_verify_sig_");
    if (!sig_path) {
        ret = -1;
        goto out;
    }

    cert_path = tempnam(P_tmpdir, "ra_tls_verify_cert_");
    if (!cert_path) {
        ret = -1;
        goto out;
    }

    advisory_path = tempnam(P_tmpdir, "ra_tls_verify_advisory_");
    if (!advisory_path) {
        ret = -1;
        goto out;
    }

    ret = ias_verify_quote(ias, quote, quote_size, nonce, report_path, sig_path, cert_path,
                           advisory_path);
    if (ret < 0) {
        ERROR("Failed to submit quote to IAS\n");
        ret = -1;
        goto out;
    }

    /* TODO: obtain cert revocation lists via ias_get_sigrl(); currently they are not used during
     *       IAS attestation report verification, so we don't obtain them */

    /* read all components of the IAS attestation report from temporary files and verify them */
    ssize_t report_size = 0;
    report_blob = read_file(report_path, &report_size);
    if (!report_blob) {
        ERROR("Failed to read report file '%s'\n", report_path);
        return -1;
    }

    ssize_t sig_size = 0;
    sig_blob = read_file(sig_path, &sig_size);
    if (!sig_blob) {
        ERROR("Failed to read report signature file '%s'\n", sig_path);
        return -1;
    }

    bool allow_outdated_tcb;
    const char* mrsigner;
    const char* mrenclave;
    const char* isv_prod_id;
    const char* isv_svn;
    const char* ias_pub_key_pem;

    ret = getenv_allow_outdated_tcb(&allow_outdated_tcb);
    if (ret < 0) {
        ERROR("Failed to obtain environment variable" RA_TLS_ALLOW_OUTDATED_TCB "\n");
        ret = -1;
        goto out;
    }

    ret = getenv_enclave_measurements(&mrsigner, &mrenclave, &isv_prod_id, &isv_svn);
    if (ret < 0) {
        ERROR("Failed to obtain environment variables for enclave measurements\n");
        ret = -1;
        goto out;
    }

    ret = getenv_ias_pub_key_pem(&ias_pub_key_pem);
    if (ret < 0) {
        ERROR("Failed to obtain environment variable" RA_TLS_IAS_PUB_KEY_PEM "\n");
        ret = -1;
        goto out;
    }

    ret = verify_ias_report(report_blob, report_size, sig_blob, sig_size, allow_outdated_tcb,
                            nonce, mrsigner, mrenclave, isv_prod_id, isv_svn,
                            (char*)quote->report_body.report_data.d, ias_pub_key_pem);
    if (ret < 0) {
        ERROR("Failed to verify attestation report from IAS\n");
        ret = -1;
        goto out;
    }

    ret = 0;
out:
    if (ias)
        ias_cleanup(ias);

    if (report_path) {
        unlink(report_path);
        free(report_path);
    }
    if (sig_path) {
        unlink(sig_path);
        free(sig_path);
    }
    if (cert_path) {
        unlink(cert_path);
        free(cert_path);
    }
    if (advisory_path) {
        unlink(advisory_path);
        free(advisory_path);
    }

    free(report_blob);
    free(sig_blob);

    return ret;
}

int ra_tls_verify_callback_der(uint8_t *der_crt, size_t der_crt_size) {
    int ret;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);

    ret = mbedtls_x509_crt_parse(&crt, der_crt, der_crt_size);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = ra_tls_verify_callback(/*unused data=*/NULL, &crt, /*depth=*/0, /*flags=*/NULL);
    if (ret < 0) {
        ret = -1;
        goto out;
    }

    ret = 0;
out:
    mbedtls_x509_crt_free(&crt);
    return ret;
}
