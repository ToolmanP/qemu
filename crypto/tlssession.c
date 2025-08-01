/*
 * QEMU crypto TLS session support
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "crypto/tlssession.h"
#include "crypto/tlscredsanon.h"
#include "crypto/tlscredspsk.h"
#include "crypto/tlscredsx509.h"
#include "qapi/error.h"
#include "authz/base.h"
#include "tlscredspriv.h"
#include "trace.h"

#ifdef CONFIG_GNUTLS


#include <gnutls/x509.h>


struct QCryptoTLSSession {
    QCryptoTLSCreds *creds;
    gnutls_session_t handle;
    char *hostname;
    char *authzid;
    bool handshakeComplete;
    QCryptoTLSSessionWriteFunc writeFunc;
    QCryptoTLSSessionReadFunc readFunc;
    void *opaque;
    char *peername;

    /*
     * Allow concurrent reads and writes, so track
     * errors separately
     */
    Error *rerr;
    Error *werr;

    /*
     * Used to protect against broken GNUTLS thread safety
     * https://gitlab.com/gnutls/gnutls/-/issues/1717
     */
    bool requireThreadSafety;
    bool lockEnabled;
    QemuMutex lock;
};


void
qcrypto_tls_session_free(QCryptoTLSSession *session)
{
    if (!session) {
        return;
    }

    error_free(session->rerr);
    error_free(session->werr);

    gnutls_deinit(session->handle);
    g_free(session->hostname);
    g_free(session->peername);
    g_free(session->authzid);
    object_unref(OBJECT(session->creds));
    qemu_mutex_destroy(&session->lock);
    g_free(session);
}


static ssize_t
qcrypto_tls_session_push(void *opaque, const void *buf, size_t len)
{
    QCryptoTLSSession *session = opaque;
    ssize_t ret;

    if (!session->writeFunc) {
        errno = EIO;
        return -1;
    };

    if (session->lockEnabled) {
        qemu_mutex_unlock(&session->lock);
    }

    error_free(session->werr);
    session->werr = NULL;

    ret = session->writeFunc(buf, len, session->opaque, &session->werr);

    if (session->lockEnabled) {
        qemu_mutex_lock(&session->lock);
    }

    if (ret == QCRYPTO_TLS_SESSION_ERR_BLOCK) {
        errno = EAGAIN;
        return -1;
    } else if (ret < 0) {
        errno = EIO;
        return -1;
    } else {
        return ret;
    }
}


static ssize_t
qcrypto_tls_session_pull(void *opaque, void *buf, size_t len)
{
    QCryptoTLSSession *session = opaque;
    ssize_t ret;

    if (!session->readFunc) {
        errno = EIO;
        return -1;
    };

    error_free(session->rerr);
    session->rerr = NULL;

    if (session->lockEnabled) {
        qemu_mutex_unlock(&session->lock);
    }

    ret = session->readFunc(buf, len, session->opaque, &session->rerr);

    if (session->lockEnabled) {
        qemu_mutex_lock(&session->lock);
    }

    if (ret == QCRYPTO_TLS_SESSION_ERR_BLOCK) {
        errno = EAGAIN;
        return -1;
    } else if (ret < 0) {
        errno = EIO;
        return -1;
    } else {
        return ret;
    }
}

#define TLS_PRIORITY_ADDITIONAL_ANON "+ANON-DH"
#define TLS_PRIORITY_ADDITIONAL_PSK "+ECDHE-PSK:+DHE-PSK:+PSK"

QCryptoTLSSession *
qcrypto_tls_session_new(QCryptoTLSCreds *creds,
                        const char *hostname,
                        const char *authzid,
                        QCryptoTLSCredsEndpoint endpoint,
                        Error **errp)
{
    QCryptoTLSSession *session;
    int ret;

    session = g_new0(QCryptoTLSSession, 1);
    trace_qcrypto_tls_session_new(
        session, creds, hostname ? hostname : "<none>",
        authzid ? authzid : "<none>", endpoint);

    if (hostname) {
        session->hostname = g_strdup(hostname);
    }
    if (authzid) {
        session->authzid = g_strdup(authzid);
    }
    session->creds = creds;
    object_ref(OBJECT(creds));

    qemu_mutex_init(&session->lock);

    if (creds->endpoint != endpoint) {
        error_setg(errp, "Credentials endpoint doesn't match session");
        goto error;
    }

    if (endpoint == QCRYPTO_TLS_CREDS_ENDPOINT_SERVER) {
        ret = gnutls_init(&session->handle, GNUTLS_SERVER);
    } else {
        ret = gnutls_init(&session->handle, GNUTLS_CLIENT);
    }
    if (ret < 0) {
        error_setg(errp, "Cannot initialize TLS session: %s",
                   gnutls_strerror(ret));
        goto error;
    }

    if (object_dynamic_cast(OBJECT(creds),
                            TYPE_QCRYPTO_TLS_CREDS_ANON)) {
        QCryptoTLSCredsAnon *acreds = QCRYPTO_TLS_CREDS_ANON(creds);
        char *prio;

        if (creds->priority != NULL) {
            prio = g_strdup_printf("%s:%s",
                                   creds->priority,
                                   TLS_PRIORITY_ADDITIONAL_ANON);
        } else {
            prio = g_strdup(CONFIG_TLS_PRIORITY ":"
                            TLS_PRIORITY_ADDITIONAL_ANON);
        }

        ret = gnutls_priority_set_direct(session->handle, prio, NULL);
        if (ret < 0) {
            error_setg(errp, "Unable to set TLS session priority %s: %s",
                       prio, gnutls_strerror(ret));
            g_free(prio);
            goto error;
        }
        g_free(prio);
        if (creds->endpoint == QCRYPTO_TLS_CREDS_ENDPOINT_SERVER) {
            ret = gnutls_credentials_set(session->handle,
                                         GNUTLS_CRD_ANON,
                                         acreds->data.server);
        } else {
            ret = gnutls_credentials_set(session->handle,
                                         GNUTLS_CRD_ANON,
                                         acreds->data.client);
        }
        if (ret < 0) {
            error_setg(errp, "Cannot set session credentials: %s",
                       gnutls_strerror(ret));
            goto error;
        }
    } else if (object_dynamic_cast(OBJECT(creds),
                                   TYPE_QCRYPTO_TLS_CREDS_PSK)) {
        QCryptoTLSCredsPSK *pcreds = QCRYPTO_TLS_CREDS_PSK(creds);
        char *prio;

        if (creds->priority != NULL) {
            prio = g_strdup_printf("%s:%s",
                                   creds->priority,
                                   TLS_PRIORITY_ADDITIONAL_PSK);
        } else {
            prio = g_strdup(CONFIG_TLS_PRIORITY ":"
                            TLS_PRIORITY_ADDITIONAL_PSK);
        }

        ret = gnutls_priority_set_direct(session->handle, prio, NULL);
        if (ret < 0) {
            error_setg(errp, "Unable to set TLS session priority %s: %s",
                       prio, gnutls_strerror(ret));
            g_free(prio);
            goto error;
        }
        g_free(prio);
        if (creds->endpoint == QCRYPTO_TLS_CREDS_ENDPOINT_SERVER) {
            ret = gnutls_credentials_set(session->handle,
                                         GNUTLS_CRD_PSK,
                                         pcreds->data.server);
        } else {
            ret = gnutls_credentials_set(session->handle,
                                         GNUTLS_CRD_PSK,
                                         pcreds->data.client);
        }
        if (ret < 0) {
            error_setg(errp, "Cannot set session credentials: %s",
                       gnutls_strerror(ret));
            goto error;
        }
    } else if (object_dynamic_cast(OBJECT(creds),
                                   TYPE_QCRYPTO_TLS_CREDS_X509)) {
        QCryptoTLSCredsX509 *tcreds = QCRYPTO_TLS_CREDS_X509(creds);
        const char *prio = creds->priority;
        if (!prio) {
            prio = CONFIG_TLS_PRIORITY;
        }

        ret = gnutls_priority_set_direct(session->handle, prio, NULL);
        if (ret < 0) {
            error_setg(errp, "Cannot set default TLS session priority %s: %s",
                       prio, gnutls_strerror(ret));
            goto error;
        }
        ret = gnutls_credentials_set(session->handle,
                                     GNUTLS_CRD_CERTIFICATE,
                                     tcreds->data);
        if (ret < 0) {
            error_setg(errp, "Cannot set session credentials: %s",
                       gnutls_strerror(ret));
            goto error;
        }

        if (creds->endpoint == QCRYPTO_TLS_CREDS_ENDPOINT_SERVER) {
            /* This requests, but does not enforce a client cert.
             * The cert checking code later does enforcement */
            gnutls_certificate_server_set_request(session->handle,
                                                  GNUTLS_CERT_REQUEST);
        }
    } else {
        error_setg(errp, "Unsupported TLS credentials type %s",
                   object_get_typename(OBJECT(creds)));
        goto error;
    }

    gnutls_transport_set_ptr(session->handle, session);
    gnutls_transport_set_push_function(session->handle,
                                       qcrypto_tls_session_push);
    gnutls_transport_set_pull_function(session->handle,
                                       qcrypto_tls_session_pull);

    return session;

 error:
    qcrypto_tls_session_free(session);
    return NULL;
}

void qcrypto_tls_session_require_thread_safety(QCryptoTLSSession *sess)
{
    sess->requireThreadSafety = true;
}

static int
qcrypto_tls_session_check_certificate(QCryptoTLSSession *session,
                                      Error **errp)
{
    int ret;
    unsigned int status;
    const gnutls_datum_t *certs;
    unsigned int nCerts, i;
    time_t now;
    gnutls_x509_crt_t cert = NULL;
    Error *err = NULL;

    now = time(NULL);
    if (now == ((time_t)-1)) {
        error_setg_errno(errp, errno, "Cannot get current time");
        return -1;
    }

    ret = gnutls_certificate_verify_peers2(session->handle, &status);
    if (ret < 0) {
        error_setg(errp, "Verify failed: %s", gnutls_strerror(ret));
        return -1;
    }

    if (status != 0) {
        const char *reason = "Invalid certificate";

        if (status & GNUTLS_CERT_INVALID) {
            reason = "The certificate is not trusted";
        }

        if (status & GNUTLS_CERT_SIGNER_NOT_FOUND) {
            reason = "The certificate hasn't got a known issuer";
        }

        if (status & GNUTLS_CERT_REVOKED) {
            reason = "The certificate has been revoked";
        }

        if (status & GNUTLS_CERT_INSECURE_ALGORITHM) {
            reason = "The certificate uses an insecure algorithm";
        }

        error_setg(errp, "%s", reason);
        return -1;
    }

    certs = gnutls_certificate_get_peers(session->handle, &nCerts);
    if (!certs) {
        error_setg(errp, "No certificate peers");
        return -1;
    }

    for (i = 0; i < nCerts; i++) {
        ret = gnutls_x509_crt_init(&cert);
        if (ret < 0) {
            error_setg(errp, "Cannot initialize certificate: %s",
                       gnutls_strerror(ret));
            return -1;
        }

        ret = gnutls_x509_crt_import(cert, &certs[i], GNUTLS_X509_FMT_DER);
        if (ret < 0) {
            error_setg(errp, "Cannot import certificate: %s",
                       gnutls_strerror(ret));
            goto error;
        }

        if (gnutls_x509_crt_get_expiration_time(cert) < now) {
            error_setg(errp, "The certificate has expired");
            goto error;
        }

        if (gnutls_x509_crt_get_activation_time(cert) > now) {
            error_setg(errp, "The certificate is not yet activated");
            goto error;
        }

        if (gnutls_x509_crt_get_activation_time(cert) > now) {
            error_setg(errp, "The certificate is not yet activated");
            goto error;
        }

        if (i == 0) {
            size_t dnameSize = 1024;
            session->peername = g_malloc(dnameSize);
        requery:
            ret = gnutls_x509_crt_get_dn(cert, session->peername, &dnameSize);
            if (ret < 0) {
                if (ret == GNUTLS_E_SHORT_MEMORY_BUFFER) {
                    session->peername = g_realloc(session->peername,
                                                  dnameSize);
                    goto requery;
                }
                error_setg(errp, "Cannot get client distinguished name: %s",
                           gnutls_strerror(ret));
                goto error;
            }
            if (session->authzid) {
                bool allow;

                allow = qauthz_is_allowed_by_id(session->authzid,
                                                session->peername, &err);
                if (err) {
                    error_propagate(errp, err);
                    goto error;
                }
                if (!allow) {
                    error_setg(errp, "TLS x509 authz check for %s is denied",
                               session->peername);
                    goto error;
                }
            }
            if (session->hostname) {
                if (!gnutls_x509_crt_check_hostname(cert, session->hostname)) {
                    error_setg(errp,
                               "Certificate does not match the hostname %s",
                               session->hostname);
                    goto error;
                }
            } else {
                if (session->creds->endpoint ==
                    QCRYPTO_TLS_CREDS_ENDPOINT_CLIENT) {
                    error_setg(errp, "No hostname for certificate validation");
                    goto error;
                }
            }
        }

        gnutls_x509_crt_deinit(cert);
    }

    return 0;

 error:
    gnutls_x509_crt_deinit(cert);
    return -1;
}


int
qcrypto_tls_session_check_credentials(QCryptoTLSSession *session,
                                      Error **errp)
{
    if (object_dynamic_cast(OBJECT(session->creds),
                            TYPE_QCRYPTO_TLS_CREDS_ANON)) {
        trace_qcrypto_tls_session_check_creds(session, "nop");
        return 0;
    } else if (object_dynamic_cast(OBJECT(session->creds),
                            TYPE_QCRYPTO_TLS_CREDS_PSK)) {
        trace_qcrypto_tls_session_check_creds(session, "nop");
        return 0;
    } else if (object_dynamic_cast(OBJECT(session->creds),
                            TYPE_QCRYPTO_TLS_CREDS_X509)) {
        if (session->creds->verifyPeer) {
            int ret = qcrypto_tls_session_check_certificate(session,
                                                            errp);
            trace_qcrypto_tls_session_check_creds(session,
                                                  ret == 0 ? "pass" : "fail");
            return ret;
        } else {
            trace_qcrypto_tls_session_check_creds(session, "skip");
            return 0;
        }
    } else {
        trace_qcrypto_tls_session_check_creds(session, "error");
        error_setg(errp, "Unexpected credential type %s",
                   object_get_typename(OBJECT(session->creds)));
        return -1;
    }
}


void
qcrypto_tls_session_set_callbacks(QCryptoTLSSession *session,
                                  QCryptoTLSSessionWriteFunc writeFunc,
                                  QCryptoTLSSessionReadFunc readFunc,
                                  void *opaque)
{
    session->writeFunc = writeFunc;
    session->readFunc = readFunc;
    session->opaque = opaque;
}


ssize_t
qcrypto_tls_session_write(QCryptoTLSSession *session,
                          const char *buf,
                          size_t len,
                          Error **errp)
{
    ssize_t ret;

    if (session->lockEnabled) {
        qemu_mutex_lock(&session->lock);
    }

    ret = gnutls_record_send(session->handle, buf, len);

    if (session->lockEnabled) {
        qemu_mutex_unlock(&session->lock);
    }

    if (ret < 0) {
        if (ret == GNUTLS_E_AGAIN) {
            return QCRYPTO_TLS_SESSION_ERR_BLOCK;
        } else {
            if (session->werr) {
                error_propagate(errp, session->werr);
                session->werr = NULL;
            } else {
                error_setg(errp,
                           "Cannot write to TLS channel: %s",
                           gnutls_strerror(ret));
            }
            return -1;
        }
    }

    return ret;
}


ssize_t
qcrypto_tls_session_read(QCryptoTLSSession *session,
                         char *buf,
                         size_t len,
                         bool gracefulTermination,
                         Error **errp)
{
    ssize_t ret;

    if (session->lockEnabled) {
        qemu_mutex_lock(&session->lock);
    }

    ret = gnutls_record_recv(session->handle, buf, len);

    if (session->lockEnabled) {
        qemu_mutex_unlock(&session->lock);
    }

    if (ret < 0) {
        if (ret == GNUTLS_E_AGAIN) {
            return QCRYPTO_TLS_SESSION_ERR_BLOCK;
        } else if ((ret == GNUTLS_E_PREMATURE_TERMINATION) &&
                   gracefulTermination){
            return 0;
        } else {
            if (session->rerr) {
                error_propagate(errp, session->rerr);
                session->rerr = NULL;
            } else {
                error_setg(errp,
                           "Cannot read from TLS channel: %s",
                           gnutls_strerror(ret));
            }
            return -1;
        }
    }

    return ret;
}


size_t
qcrypto_tls_session_check_pending(QCryptoTLSSession *session)
{
    return gnutls_record_check_pending(session->handle);
}


int
qcrypto_tls_session_handshake(QCryptoTLSSession *session,
                              Error **errp)
{
    int ret;
    ret = gnutls_handshake(session->handle);

    if (!ret) {
#ifdef CONFIG_GNUTLS_BUG1717_WORKAROUND
        gnutls_cipher_algorithm_t cipher =
            gnutls_cipher_get(session->handle);

        /*
         * Any use of rekeying in TLS 1.3 is unsafe for
         * a gnutls with bug 1717, however, we know that
         * QEMU won't initiate manual rekeying. Thus we
         * only have to protect against automatic rekeying
         * which doesn't trigger with CHACHA20
         */
        trace_qcrypto_tls_session_parameters(
            session,
            session->requireThreadSafety,
            gnutls_protocol_get_version(session->handle),
            cipher);

        if (session->requireThreadSafety &&
            gnutls_protocol_get_version(session->handle) ==
            GNUTLS_TLS1_3 &&
            cipher != GNUTLS_CIPHER_CHACHA20_POLY1305) {
            warn_report("WARNING: activating thread safety countermeasures "
                        "for potentially broken GNUTLS with TLS1.3 cipher=%d",
                        cipher);
            trace_qcrypto_tls_session_bug1717_workaround(session);
            session->lockEnabled = true;
        }
#endif

        session->handshakeComplete = true;
        return QCRYPTO_TLS_HANDSHAKE_COMPLETE;
    }

    if (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN) {
        int direction = gnutls_record_get_direction(session->handle);
        return direction ? QCRYPTO_TLS_HANDSHAKE_SENDING :
            QCRYPTO_TLS_HANDSHAKE_RECVING;
    }

    if (session->rerr || session->werr) {
        error_setg(errp, "TLS handshake failed: %s: %s",
                   gnutls_strerror(ret),
                   error_get_pretty(session->rerr ?
                                    session->rerr : session->werr));
    } else {
        error_setg(errp, "TLS handshake failed: %s",
                   gnutls_strerror(ret));
    }

    error_free(session->rerr);
    error_free(session->werr);
    session->rerr = session->werr = NULL;

    return -1;
}


int
qcrypto_tls_session_bye(QCryptoTLSSession *session, Error **errp)
{
    int ret;

    if (!session->handshakeComplete) {
        return 0;
    }

    if (session->lockEnabled) {
        qemu_mutex_lock(&session->lock);
    }
    ret = gnutls_bye(session->handle, GNUTLS_SHUT_WR);

    if (session->lockEnabled) {
        qemu_mutex_unlock(&session->lock);
    }

    if (!ret) {
        return QCRYPTO_TLS_BYE_COMPLETE;
    }

    if (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN) {
        int direction = gnutls_record_get_direction(session->handle);
        return direction ? QCRYPTO_TLS_BYE_SENDING : QCRYPTO_TLS_BYE_RECVING;
    }

    if (session->rerr || session->werr) {
        error_setg(errp, "TLS termination failed: %s: %s", gnutls_strerror(ret),
                   error_get_pretty(session->rerr ?
                                    session->rerr : session->werr));
    } else {
        error_setg(errp, "TLS termination failed: %s", gnutls_strerror(ret));
    }

    error_free(session->rerr);
    error_free(session->werr);
    session->rerr = session->werr = NULL;

    return -1;
}

int
qcrypto_tls_session_get_key_size(QCryptoTLSSession *session,
                                 Error **errp)
{
    gnutls_cipher_algorithm_t cipher;
    int ssf;

    cipher = gnutls_cipher_get(session->handle);
    ssf = gnutls_cipher_get_key_size(cipher);
    if (!ssf) {
        error_setg(errp, "Cannot get TLS cipher key size");
        return -1;
    }
    return ssf;
}


char *
qcrypto_tls_session_get_peer_name(QCryptoTLSSession *session)
{
    if (session->peername) {
        return g_strdup(session->peername);
    }
    return NULL;
}


#else /* ! CONFIG_GNUTLS */


QCryptoTLSSession *
qcrypto_tls_session_new(QCryptoTLSCreds *creds G_GNUC_UNUSED,
                        const char *hostname G_GNUC_UNUSED,
                        const char *authzid G_GNUC_UNUSED,
                        QCryptoTLSCredsEndpoint endpoint G_GNUC_UNUSED,
                        Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return NULL;
}

void qcrypto_tls_session_require_thread_safety(QCryptoTLSSession *sess)
{
}

void
qcrypto_tls_session_free(QCryptoTLSSession *sess G_GNUC_UNUSED)
{
}


int
qcrypto_tls_session_check_credentials(QCryptoTLSSession *sess G_GNUC_UNUSED,
                                      Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return -1;
}


void
qcrypto_tls_session_set_callbacks(
    QCryptoTLSSession *sess G_GNUC_UNUSED,
    QCryptoTLSSessionWriteFunc writeFunc G_GNUC_UNUSED,
    QCryptoTLSSessionReadFunc readFunc G_GNUC_UNUSED,
    void *opaque G_GNUC_UNUSED)
{
}


ssize_t
qcrypto_tls_session_write(QCryptoTLSSession *sess,
                          const char *buf,
                          size_t len,
                          Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return -1;
}


ssize_t
qcrypto_tls_session_read(QCryptoTLSSession *sess,
                         char *buf,
                         size_t len,
                         bool gracefulTermination,
                         Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return -1;
}


size_t
qcrypto_tls_session_check_pending(QCryptoTLSSession *session)
{
    return 0;
}


int
qcrypto_tls_session_handshake(QCryptoTLSSession *sess,
                              Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return -1;
}


int
qcrypto_tls_session_bye(QCryptoTLSSession *session, Error **errp)
{
    return QCRYPTO_TLS_BYE_COMPLETE;
}


int
qcrypto_tls_session_get_key_size(QCryptoTLSSession *sess,
                                 Error **errp)
{
    error_setg(errp, "TLS requires GNUTLS support");
    return -1;
}


char *
qcrypto_tls_session_get_peer_name(QCryptoTLSSession *sess)
{
    return NULL;
}

#endif
