/*-------------------------------------------------------------------------
 *
 * be-secure.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure.c
 *
 *	  Since the server static private key ($DataDir/server.key)
 *	  will normally be stored unencrypted so that the database
 *	  backend can restart automatically, it is important that
 *	  we select an algorithm that continues to provide confidentiality
 *	  even if the attacker has the server's private key.  Ephemeral
 *	  DH (EDH) keys provide this and more (Perfect Forward Secrecy
 *	  aka PFS).
 *
 *	  N.B., the static private key should still be protected to
 *	  the largest extent possible, to minimize the risk of
 *	  impersonations.
 *
 *	  Another benefit of EDH is that it allows the backend and
 *	  clients to use DSA keys.  DSA keys can only provide digital
 *	  signatures, not encryption, and are often acceptable in
 *	  jurisdictions where RSA keys are unacceptable.
 *
 *	  The downside to EDH is that it makes it impossible to
 *	  use ssldump(1) if there's a problem establishing an SSL
 *	  session.  In this case you'll need to temporarily disable
 *	  EDH by commenting out the callback.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include "miscadmin.h"

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/dh.h>
#if OPENSSL_VERSION_NUMBER >= 0x0907000L
#include <openssl/conf.h>
#endif
#if (OPENSSL_VERSION_NUMBER >= 0x0090800fL) && !defined(OPENSSL_NO_ECDH)
#include <openssl/ec.h>
#endif
#endif   /* USE_SSL */

#include "libpq/libpq.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#define ERROR_BUF_SIZE 32

#ifdef USE_SSL

static DH  *load_dh_file(int keylength);
static DH  *load_dh_buffer(const char *, size_t);
static DH  *generate_dh_parameters(int prime_len, int generator);
static DH  *tmp_dh_cb(SSL *s, int is_export, int keylength);
static int	verify_cb(int, X509_STORE_CTX *);
static void info_cb(const SSL *ssl, int type, int args);
int be_tls_init(bool failOnError);
static int	open_server_SSL(Port *);
static void close_SSL(Port *);
static const char *SSLerrmessage(unsigned long ecode);
#endif

static bool SSL_initialized = false;

char	   *ssl_cert_file;
char	   *ssl_key_file;
char	   *ssl_ca_file;
char	   *ssl_crl_file;

#ifdef USE_SSL
static bool initialize_ecdh(SSL_CTX *context, bool failOnError);
static SSL_CTX *SSL_context = NULL;
static bool ssl_loaded_verify_locations = false;
#endif

/* GUC variable controlling SSL cipher list */
char	   *SSLCipherSuites = NULL;

/* GUC variable for default ECHD curve. */
char	   *SSLECDHCurve;

/* GUC variable: if false, prefer client ciphers */
bool		SSLPreferServerCiphers;

/* ------------------------------------------------------------ */
/*						 Hardcoded values						*/
/* ------------------------------------------------------------ */

/*
 *	Hardcoded DH parameters, used in ephemeral DH keying.
 *	As discussed above, EDH protects the confidentiality of
 *	sessions even if the static private key is compromised,
 *	so we are *highly* motivated to ensure that we can use
 *	EDH even if the DBA... or an attacker... deletes the
 *	$DataDir/dh*.pem files.
 *
 *	We could refuse SSL connections unless a good DH parameter
 *	file exists, but some clients may quietly renegotiate an
 *	unsecured connection without fully informing the user.
 *	Very uncool.
 *
 *	Alternatively, the backend could attempt to load these files
 *	on startup if SSL is enabled - and refuse to start if any
 *	do not exist - but this would tend to piss off DBAs.
 *
 *	If you want to create your own hardcoded DH parameters
 *	for fun and profit, review "Assigned Number for SKIP
 *	Protocols" (http://www.skip-vpn.org/spec/numbers.html)
 *	for suggestions.
 */
#ifdef USE_SSL

static const char file_dh512[] =
"-----BEGIN DH PARAMETERS-----\n\
MEYCQQD1Kv884bEpQBgRjXyEpwpy1obEAxnIByl6ypUM2Zafq9AKUJsCRtMIPWak\n\
XUGfnHy9iUsiGSa6q6Jew1XpKgVfAgEC\n\
-----END DH PARAMETERS-----\n";

static const char file_dh1024[] =
"-----BEGIN DH PARAMETERS-----\n\
MIGHAoGBAPSI/VhOSdvNILSd5JEHNmszbDgNRR0PfIizHHxbLY7288kjwEPwpVsY\n\
jY67VYy4XTjTNP18F1dDox0YbN4zISy1Kv884bEpQBgRjXyEpwpy1obEAxnIByl6\n\
ypUM2Zafq9AKUJsCRtMIPWakXUGfnHy9iUsiGSa6q6Jew1XpL3jHAgEC\n\
-----END DH PARAMETERS-----\n";

static const char file_dh2048[] =
"-----BEGIN DH PARAMETERS-----\n\
MIIBCAKCAQEA9kJXtwh/CBdyorrWqULzBej5UxE5T7bxbrlLOCDaAadWoxTpj0BV\n\
89AHxstDqZSt90xkhkn4DIO9ZekX1KHTUPj1WV/cdlJPPT2N286Z4VeSWc39uK50\n\
T8X8dryDxUcwYc58yWb/Ffm7/ZFexwGq01uejaClcjrUGvC/RgBYK+X0iP1YTknb\n\
zSC0neSRBzZrM2w4DUUdD3yIsxx8Wy2O9vPJI8BD8KVbGI2Ou1WMuF040zT9fBdX\n\
Q6MdGGzeMyEstSr/POGxKUAYEY18hKcKctaGxAMZyAcpesqVDNmWn6vQClCbAkbT\n\
CD1mpF1Bn5x8vYlLIhkmuquiXsNV6TILOwIBAg==\n\
-----END DH PARAMETERS-----\n";

static const char file_dh4096[] =
"-----BEGIN DH PARAMETERS-----\n\
MIICCAKCAgEA+hRyUsFN4VpJ1O8JLcCo/VWr19k3BCgJ4uk+d+KhehjdRqNDNyOQ\n\
l/MOyQNQfWXPeGKmOmIig6Ev/nm6Nf9Z2B1h3R4hExf+zTiHnvVPeRBhjdQi81rt\n\
Xeoh6TNrSBIKIHfUJWBh3va0TxxjQIs6IZOLeVNRLMqzeylWqMf49HsIXqbcokUS\n\
Vt1BkvLdW48j8PPv5DsKRN3tloTxqDJGo9tKvj1Fuk74A+Xda1kNhB7KFlqMyN98\n\
VETEJ6c7KpfOo30mnK30wqw3S8OtaIR/maYX72tGOno2ehFDkq3pnPtEbD2CScxc\n\
alJC+EL7RPk5c/tgeTvCngvc1KZn92Y//EI7G9tPZtylj2b56sHtMftIoYJ9+ODM\n\
sccD5Piz/rejE3Ome8EOOceUSCYAhXn8b3qvxVI1ddd1pED6FHRhFvLrZxFvBEM9\n\
ERRMp5QqOaHJkM+Dxv8Cj6MqrCbfC4u+ZErxodzuusgDgvZiLF22uxMZbobFWyte\n\
OvOzKGtwcTqO/1wV5gKkzu1ZVswVUQd5Gg8lJicwqRWyyNRczDDoG9jVDxmogKTH\n\
AaqLulO7R8Ifa1SwF2DteSGVtgWEN8gDpN3RBmmPTDngyF2DHb5qmpnznwtFKdTL\n\
KWbuHn491xNO25CQWMtem80uKw+pTnisBRF/454n1Jnhub144YRBoN8CAQI=\n\
-----END DH PARAMETERS-----\n";
#endif

/* ------------------------------------------------------------ */
/*			 Procedures common to all secure sessions			*/
/* ------------------------------------------------------------ */

/*
 *	Initialize global context
 */
int
secure_initialize(bool failOnError)
{
#ifdef USE_SSL
    return be_tls_init(failOnError);
#else
    return 0;
#endif
}

void
secure_destroy(void)
{
#ifdef USE_SSL
    be_tls_destroy();
#endif
}

/*
 * Indicate if we have loaded the root CA store to verify certificates
 */
bool
secure_loaded_verify_locations(void)
{
#ifdef USE_SSL
    return ssl_loaded_verify_locations;
#else
    return false;
#endif
}

/*
 *	Attempt to negotiate secure session.
 */
int
secure_open_server(Port *port)
{
    int			r = 0;

#ifdef USE_SSL
    r = open_server_SSL(port);
#endif
    return r;
}

/*
 *	Close secure session.
 */
void
secure_close(Port *port)
{
#ifdef USE_SSL
    if (port->ssl)
		close_SSL(port);
#endif
}

/*
 *	Read data from a secure connection.
 */
ssize_t
secure_read(Port *port, void *ptr, size_t len)
{
    ssize_t		n;

#ifdef USE_SSL
    if (port->ssl)
	{
		int			err;
		unsigned long ecode;

rloop:
		errno = 0;
		ERR_clear_error();
		n = SSL_read(port->ssl, ptr, len);
		err = SSL_get_error(port->ssl, n);
		ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
		switch (err)
		{
			case SSL_ERROR_NONE:
				port->count += n;
				break;
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				if (port->noblock)
				{
					errno = EWOULDBLOCK;
					n = -1;
					break;
				}
#ifdef WIN32
				pgwin32_waitforsinglesocket(SSL_get_fd(port->ssl),
											(err == SSL_ERROR_WANT_READ) ?
									FD_READ | FD_CLOSE : FD_WRITE | FD_CLOSE,
											INFINITE);
#endif
				goto rloop;
			case SSL_ERROR_SYSCALL:
				/* leave it to caller to ereport the value of errno */
				if (n != -1)
				{
					errno = ECONNRESET;
					n = -1;
				}
				break;
			case SSL_ERROR_SSL:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("SSL error: %s", SSLerrmessage(ecode))));
				errno = ECONNRESET;
				n = -1;
				break;
			case SSL_ERROR_ZERO_RETURN:
				/* connection was cleanly shut down by peer */
				n = 0;
				break;
			default:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unrecognized SSL error code: %d",
								err)));
				errno = ECONNRESET;
				n = -1;
				break;
		}
	}
	else
#endif
    {
        prepare_for_client_read();

        n = recv(port->sock, ptr, len, 0);

        client_read_ended();
    }

    return n;
}

/*
 * Report a COMMERROR.
 *
 * This function holds an interrupt before reporting this error to avoid
 * a self deadlock situation, see MPP-13718 for more info.
 */
#ifdef USE_SSL
static void
report_commerror(const char *err_msg)
{
	HOLD_INTERRUPTS();

	ereport(COMMERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("%s",err_msg)));

	RESUME_INTERRUPTS();
}
#endif

/*
 *	Write data to a secure connection.
 */
ssize_t
secure_write(Port *port, void *ptr, size_t len)
{
    ssize_t		n;

#ifdef USE_SSL
    if (port->ssl)
	{
		int			err;
		unsigned long ecode;

wloop:
		errno = 0;
		ERR_clear_error();
		n = SSL_write(port->ssl, ptr, len);
		err = SSL_get_error(port->ssl, n);

		const int ERR_MSG_LEN = ERROR_BUF_SIZE + 20;
		char err_msg[ERR_MSG_LEN];

		ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
		switch (err)
		{
			case SSL_ERROR_NONE:
				port->count += n;
				break;
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
#ifdef WIN32
				pgwin32_waitforsinglesocket(SSL_get_fd(port->ssl),
											(err == SSL_ERROR_WANT_READ) ?
									FD_READ | FD_CLOSE : FD_WRITE | FD_CLOSE,
											INFINITE);
#endif
				goto wloop;
			case SSL_ERROR_SYSCALL:
				/* leave it to caller to ereport the value of errno */
				if (n != -1)
				{
					errno = ECONNRESET;
					n = -1;
				}
				break;
			case SSL_ERROR_SSL:
				snprintf((char *)&err_msg, ERR_MSG_LEN, "SSL error: %s", SSLerrmessage(ecode));
				report_commerror(err_msg);
				/* fall through */
				errno = ECONNRESET;
				n = -1;
				break;
			case SSL_ERROR_ZERO_RETURN:
				/*
				 * the SSL connnection was closed, leave it to the caller
				 * to ereport it
				 */
				errno = ECONNRESET;
				n = -1;
				break;
			default:
				snprintf((char *)&err_msg, ERR_MSG_LEN, "unrecognized SSL error code: %d", err);
				report_commerror(err_msg);

				errno = ECONNRESET;
				n = -1;
				break;
		}
	}
	else
#endif
    {
        prepare_for_client_write();
        n = send(port->sock, ptr, len, 0);
        client_write_ended();
    }

    return n;
}

/* ------------------------------------------------------------ */
/*						  SSL specific code						*/
/* ------------------------------------------------------------ */
#ifdef USE_SSL

/*
 * Private substitute BIO: this does the sending and receiving using send() and
 * recv() instead. This is so that we can enable and disable interrupts
 * just while calling recv(). We cannot have interrupts occurring while
 * the bulk of openssl runs, because it uses malloc() and possibly other
 * non-reentrant libc facilities. We also need to call send() and recv()
 * directly so it gets passed through the socket/signals layer on Win32.
 *
 * These functions are closely modelled on the standard socket BIO in OpenSSL;
 * see sock_read() and sock_write() in OpenSSL's crypto/bio/bss_sock.c.
 * XXX OpenSSL 1.0.1e considers many more errcodes than just EINTR as reasons
 * to retry; do we need to adopt their logic for that?
 */

static BIO_METHOD *my_bio_methods = NULL;

static int
my_sock_read(BIO *h, char *buf, int size)
{
	int			res = 0;

	prepare_for_client_read();

	if (buf != NULL)
	{
		res = recv(BIO_get_fd(h, NULL), buf, size, 0);
		BIO_clear_retry_flags(h);
		if (res <= 0)
		{
			/* If we were interrupted, tell caller to retry */
			if (errno == EINTR)
			{
				BIO_set_retry_read(h);
			}
		}
	}

	client_read_ended();

	return res;
}

static int
my_sock_write(BIO *h, const char *buf, int size)
{
	int			res = 0;

	prepare_for_client_write();

	res = send(BIO_get_fd(h, NULL), buf, size, 0);
	BIO_clear_retry_flags(h);
	if (res <= 0)
	{
		if (errno == EINTR)
		{
			BIO_set_retry_write(h);
		}
	}

	client_write_ended();

	return res;
}

static BIO_METHOD *
my_BIO_s_socket(void)
{
	if (!my_bio_methods)
	{
		BIO_METHOD *biom = (BIO_METHOD *) BIO_s_socket();
#ifdef HAVE_BIO_METH_NEW
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			return NULL;
		my_bio_methods = BIO_meth_new(my_bio_index, "PostgreSQL backend socket");
		if (!my_bio_methods)
			return NULL;
		if (!BIO_meth_set_write(my_bio_methods, my_sock_write) ||
			!BIO_meth_set_read(my_bio_methods, my_sock_read) ||
			!BIO_meth_set_gets(my_bio_methods, BIO_meth_get_gets(biom)) ||
			!BIO_meth_set_puts(my_bio_methods, BIO_meth_get_puts(biom)) ||
			!BIO_meth_set_ctrl(my_bio_methods, BIO_meth_get_ctrl(biom)) ||
			!BIO_meth_set_create(my_bio_methods, BIO_meth_get_create(biom)) ||
			!BIO_meth_set_destroy(my_bio_methods, BIO_meth_get_destroy(biom)) ||
			!BIO_meth_set_callback_ctrl(my_bio_methods, BIO_meth_get_callback_ctrl(biom)))
		{
			BIO_meth_free(my_bio_methods);
			my_bio_methods = NULL;
			return NULL;
		}
#else
		my_bio_methods = malloc(sizeof(BIO_METHOD));
		if (!my_bio_methods)
			return NULL;
		memcpy(my_bio_methods, biom, sizeof(BIO_METHOD));
		my_bio_methods->bread = my_sock_read;
		my_bio_methods->bwrite = my_sock_write;
#endif
	}
	return my_bio_methods;
}

/* This should exactly match openssl's SSL_set_fd except for using my BIO */
static int
my_SSL_set_fd(SSL *s, int fd)
{
	int			ret = 0;
	BIO		   *bio;
	BIO_METHOD *bio_method;

	bio_method = my_BIO_s_socket();
	if (bio_method == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	bio = BIO_new(bio_method);

	if (bio == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	BIO_set_fd(bio, fd, BIO_NOCLOSE);
	SSL_set_bio(s, bio, bio);
	ret = 1;
err:
	return ret;
}

/*
 *	Load precomputed DH parameters.
 *
 *	To prevent "downgrade" attacks, we perform a number of checks
 *	to verify that the DBA-generated DH parameters file contains
 *	what we expect it to contain.
 */
static DH  *
load_dh_file(int keylength)
{
	FILE	   *fp;
	char		fnbuf[MAXPGPATH];
	DH		   *dh = NULL;
	int			codes;

	/* attempt to open file.  It's not an error if it doesn't exist. */
	snprintf(fnbuf, sizeof(fnbuf), "dh%d.pem", keylength);
	if ((fp = fopen(fnbuf, "r")) == NULL)
		return NULL;

/*	flock(fileno(fp), LOCK_SH); */
	dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
/*	flock(fileno(fp), LOCK_UN); */
	fclose(fp);

	/* is the prime the correct size? */
	if (dh != NULL && 8 * DH_size(dh) < keylength)
	{
		elog(LOG, "DH errors (%s): %d bits expected, %d bits found",
			 fnbuf, keylength, 8 * DH_size(dh));
		dh = NULL;
	}

	/* make sure the DH parameters are usable */
	if (dh != NULL)
	{
		if (DH_check(dh, &codes) == 0)
		{
			elog(LOG, "DH_check error (%s): %s", fnbuf,
				 SSLerrmessage(ERR_get_error()));
			return NULL;
		}
		if (codes & DH_CHECK_P_NOT_PRIME)
		{
			elog(LOG, "DH error (%s): p is not prime", fnbuf);
			return NULL;
		}
		if ((codes & DH_NOT_SUITABLE_GENERATOR) &&
			(codes & DH_CHECK_P_NOT_SAFE_PRIME))
		{
			elog(LOG,
				 "DH error (%s): neither suitable generator or safe prime",
				 fnbuf);
			return NULL;
		}
	}

	return dh;
}

/*
 *	Load hardcoded DH parameters.
 *
 *	To prevent problems if the DH parameters files don't even
 *	exist, we can load DH parameters hardcoded into this file.
 */
static DH  *
load_dh_buffer(const char *buffer, size_t len)
{
	BIO		   *bio;
	DH		   *dh = NULL;

	bio = BIO_new_mem_buf((char *) buffer, len);
	if (bio == NULL)
		return NULL;
	dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
	if (dh == NULL)
		ereport(DEBUG2,
				(errmsg_internal("DH load buffer: %s",
								 SSLerrmessage(ERR_get_error()))));
	BIO_free(bio);

	return dh;
}

/*
 *	Generate DH parameters.
 *
 *	Last resort if we can't load precomputed nor hardcoded
 *	parameters.
 */
static DH  *
generate_dh_parameters(int prime_len, int generator)
{
#if (OPENSSL_VERSION_NUMBER >= 0x0090800fL)
	DH		   *dh;

	if ((dh = DH_new()) == NULL)
		return NULL;

	if (DH_generate_parameters_ex(dh, prime_len, generator, NULL))
		return dh;

	DH_free(dh);
	return NULL;
#else
	return DH_generate_parameters(prime_len, generator, NULL, NULL);
#endif
}

/*
 *	Generate an ephemeral DH key.  Because this can take a long
 *	time to compute, we can use precomputed parameters of the
 *	common key sizes.
 *
 *	Since few sites will bother to precompute these parameter
 *	files, we also provide a fallback to the parameters provided
 *	by the OpenSSL project.
 *
 *	These values can be static (once loaded or computed) since
 *	the OpenSSL library can efficiently generate random keys from
 *	the information provided.
 */
static DH  *
tmp_dh_cb(SSL *s, int is_export, int keylength)
{
	DH		   *r = NULL;
	static DH  *dh = NULL;
	static DH  *dh512 = NULL;
	static DH  *dh1024 = NULL;
	static DH  *dh2048 = NULL;
	static DH  *dh4096 = NULL;

	switch (keylength)
	{
		case 512:
			if (dh512 == NULL)
				dh512 = load_dh_file(keylength);
			if (dh512 == NULL)
				dh512 = load_dh_buffer(file_dh512, sizeof file_dh512);
			r = dh512;
			break;

		case 1024:
			if (dh1024 == NULL)
				dh1024 = load_dh_file(keylength);
			if (dh1024 == NULL)
				dh1024 = load_dh_buffer(file_dh1024, sizeof file_dh1024);
			r = dh1024;
			break;

		case 2048:
			if (dh2048 == NULL)
				dh2048 = load_dh_file(keylength);
			if (dh2048 == NULL)
				dh2048 = load_dh_buffer(file_dh2048, sizeof file_dh2048);
			r = dh2048;
			break;

		case 4096:
			if (dh4096 == NULL)
				dh4096 = load_dh_file(keylength);
			if (dh4096 == NULL)
				dh4096 = load_dh_buffer(file_dh4096, sizeof file_dh4096);
			r = dh4096;
			break;

		default:
			if (dh == NULL)
				dh = load_dh_file(keylength);
			r = dh;
	}

	/* this may take a long time, but it may be necessary... */
	if (r == NULL || 8 * DH_size(r) < keylength)
	{
		ereport(DEBUG2,
				(errmsg_internal("DH: generating parameters (%d bits)",
								 keylength)));
		r = generate_dh_parameters(keylength, DH_GENERATOR_2);
	}

	return r;
}

/*
 *	Certificate verification callback
 *
 *	This callback allows us to log intermediate problems during
 *	verification, but for now we'll see if the final error message
 *	contains enough information.
 *
 *	This callback also allows us to override the default acceptance
 *	criteria (e.g., accepting self-signed or expired certs), but
 *	for now we accept the default checks.
 */
static int
verify_cb(int ok, X509_STORE_CTX *ctx)
{
	return ok;
}

/*
 *	This callback is used to copy SSL information messages
 *	into the PostgreSQL log.
 */
static void
info_cb(const SSL *ssl, int type, int args)
{
	switch (type)
	{
		case SSL_CB_HANDSHAKE_START:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake start")));
			break;
		case SSL_CB_HANDSHAKE_DONE:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake done")));
			break;
		case SSL_CB_ACCEPT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept loop")));
			break;
		case SSL_CB_ACCEPT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept exit (%d)", args)));
			break;
		case SSL_CB_CONNECT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect loop")));
			break;
		case SSL_CB_CONNECT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect exit (%d)", args)));
			break;
		case SSL_CB_READ_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: read alert (0x%04x)", args)));
			break;
		case SSL_CB_WRITE_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: write alert (0x%04x)", args)));
			break;
	}
}


static bool
initialize_ecdh(SSL_CTX *context, bool failOnError)
{
	EC_KEY	   *ecdh;
	int			nid;
	nid = OBJ_sn2nid(SSLECDHCurve);
	if (!nid)
    {
        ereport(failOnError ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ECDH: unrecognized curve name: %s", SSLECDHCurve)));
        return false;
    }
	ecdh = EC_KEY_new_by_curve_name(nid);
	if (!ecdh)
    {
		ereport(failOnError ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ECDH: could not create key")));
		return false;
    }
	SSL_CTX_set_options(context, SSL_OP_SINGLE_ECDH_USE);
	SSL_CTX_set_tmp_ecdh(context, ecdh);
    EC_KEY_free(ecdh);
    return true;
}

/*
 *	Initialize global SSL context.
 */
int
be_tls_init(bool failOnError)
{
	STACK_OF(X509_NAME) *root_cert_list = NULL;
    SSL_CTX    *context;
    struct stat buf;
	if (!SSL_initialized)
	{
#ifdef HAVE_OPENSSL_INIT_SSL
		OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
		OPENSSL_config(NULL);
		SSL_library_init();
		SSL_load_error_strings();
#endif
        SSL_initialized = true;
    }
		/*
         * We use SSLv23_method() because it can negotiate use of the highest
         * mutually supported protocol version, while alternatives like
         * TLSv1_2_method() permit only one specific version.  Note that we don't
         * actually allow SSL v2 or v3, only TLS protocols (see below).
         */
        context = SSL_CTX_new(SSLv23_method());

        if (!context)
        {
            ereport(failOnError ? FATAL : LOG,
                    (errmsg("could not create SSL context: %s",
                            SSLerrmessage(ERR_get_error()))));
            goto error;
        }

		/*
		 * Disable OpenSSL's moving-write-buffer sanity check, because it
		 * causes unnecessary failures in nonblocking send cases.
		 */
		SSL_CTX_set_mode(context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

			/*
         * Load and verify server's certificate and private key
         */
        if (SSL_CTX_use_certificate_chain_file(context, ssl_cert_file) != 1)
        {
            ereport(failOnError ? FATAL : LOG,
                    (errcode(ERRCODE_CONFIG_FILE_ERROR),
                     errmsg("could not load server certificate file \"%s\": %s",
                            ssl_cert_file, SSLerrmessage(ERR_get_error()))));
            goto error;
        }

        if (stat(ssl_key_file, &buf) != 0)
        {
            ereport(failOnError ? FATAL : LOG,
                    (errcode_for_file_access(),
                     errmsg("could not access private key file \"%s\": %m",
                            ssl_key_file)));
            goto error;
        }

    	if (!S_ISREG(buf.st_mode))
        {
            ereport(failOnError ? FATAL : LOG,
                    (errcode(ERRCODE_CONFIG_FILE_ERROR),
                     errmsg("private key file \"%s\" is not a regular file",
                            ssl_key_file)));
            goto error;
        }

#if !defined(WIN32) && !defined(__CYGWIN__)
		if (buf.st_uid != geteuid() && buf.st_uid != 0)
        {
            ereport(failOnError ? FATAL : LOG,
                    (errcode(ERRCODE_CONFIG_FILE_ERROR),
                     errmsg("private key file \"%s\" must be owned by the database user or root",
                            ssl_key_file)));
            goto error;
        }
#endif

#if !defined(WIN32) && !defined(__CYGWIN__)
    if ((buf.st_uid == geteuid() && buf.st_mode & (S_IRWXG | S_IRWXO)) ||
        (buf.st_uid == 0 && buf.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO)))
    {
        ereport(failOnError ? FATAL : LOG,
            (errcode(ERRCODE_CONFIG_FILE_ERROR),
             errmsg("private key file \"%s\" has group or world access",
                    ssl_key_file),
             errdetail("File must have permissions u=rw (0600) or less if owned by the database user, or permissions u=rw,g=r (0640) or less if owned by root.")));
        goto error;
    }
#endif

	if (SSL_CTX_use_PrivateKey_file(context,
									ssl_key_file,
									SSL_FILETYPE_PEM) != 1)
	{
		ereport(failOnError ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not load private key file \"%s\": %s",
						ssl_key_file, SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (SSL_CTX_check_private_key(context) != 1)
	{
		ereport(failOnError ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("check of private key failed: %s",
						SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	/* set up ephemeral DH keys, and disallow SSL v2/v3 while at it */
	SSL_CTX_set_tmp_dh_callback(context, tmp_dh_cb);

	SSL_CTX_set_options(context,
						SSL_OP_SINGLE_DH_USE |
						SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

	if (!initialize_ecdh(context, failOnError))
		goto error;

	/* set up the allowed cipher list */
	if (SSL_CTX_set_cipher_list(context, SSLCipherSuites) != 1)
	{
		ereport(failOnError ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not set the cipher list (no valid ciphers available)")));
		goto error;
	}

	/* Let server choose order */
	if (SSLPreferServerCiphers)
		SSL_CTX_set_options(context, SSL_OP_CIPHER_SERVER_PREFERENCE);

	/*
	 * Load CA store, so we can verify client certificates if needed.
	 */
	if (ssl_ca_file[0])
	{
		if (SSL_CTX_load_verify_locations(context, ssl_ca_file, NULL) != 1 ||
			(root_cert_list = SSL_load_client_CA_file(ssl_ca_file)) == NULL)
			{
			    ereport(failOnError ? FATAL : LOG,
                (errcode(ERRCODE_CONFIG_FILE_ERROR),
                 errmsg("could not load root certificate file \"%s\": %s",
                        ssl_ca_file, SSLerrmessage(ERR_get_error()))));
                goto error;
		    }
	}

	/*----------
	 * Load the Certificate Revocation List (CRL).
	 * http://searchsecurity.techtarget.com/sDefinition/0,,sid14_gci803160,00.html
	 *----------
	 */
	if (ssl_crl_file[0])
	{
		X509_STORE *cvstore = SSL_CTX_get_cert_store(context);

		if (cvstore)
		{
			/* Set the flags to check against the complete CRL chain */
			if (X509_STORE_load_locations(cvstore, ssl_crl_file, NULL) == 1)
			{
				/* OpenSSL 0.96 does not support X509_V_FLAG_CRL_CHECK */
#ifdef X509_V_FLAG_CRL_CHECK
				X509_STORE_set_flags(cvstore,
						  X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
#else
				ereport(LOG,
				    (errcode(ERRCODE_CONFIG_FILE_ERROR),
                errmsg("SSL certificate revocation list file \"%s\" ignored",
				    ssl_crl_file),
                        errdetail("SSL library does not support certificate revocation lists.")));
#endif
			}
			else
            {
                ereport(failOnError ? FATAL : LOG,
                        (errcode(ERRCODE_CONFIG_FILE_ERROR),
                         errmsg("could not load SSL certificate revocation list file \"%s\": %s",
                             ssl_crl_file, SSLerrmessage(ERR_get_error()))));
                goto error;
            }
		}
	}

	if (ssl_ca_file[0])
	{
		/*
		 * Always ask for SSL client cert, but don't fail if it's not
		 * presented.  We might fail such connections later, depending on what
		 * we find in pg_hba.conf.
		 */
		SSL_CTX_set_verify(context,
						   (SSL_VERIFY_PEER |
							SSL_VERIFY_CLIENT_ONCE),
						   verify_cb);


		/*
		 * Tell OpenSSL to send the list of root certs we trust to clients in
		 * CertificateRequests.  This lets a client with a keystore select the
		 * appropriate client certificate to send to us.
		 */
		SSL_CTX_set_client_CA_list(context, root_cert_list);
	}

    /*
	 * Success!  Replace any existing SSL_context.
	 */
	if (SSL_context)
		SSL_CTX_free(SSL_context);

	SSL_context = context;

	/*
	 * Set flag to remember whether CA store has been loaded into SSL_context.
	 */
	if (ssl_ca_file[0])
		ssl_loaded_verify_locations = true;
	else
		ssl_loaded_verify_locations = false;
	return 0;

    error:
        if (context)
            SSL_CTX_free(context);
        return -1;
}

void
be_tls_destroy(void)
{
	if (SSL_context)
		SSL_CTX_free(SSL_context);
	SSL_context = NULL;
	ssl_loaded_verify_locations = false;
}



/*
 *	Attempt to negotiate SSL connection.
 */
static int
open_server_SSL(Port *port)
{
	int			r;
	int			err;
	unsigned long ecode;

	Assert(!port->ssl);
	Assert(!port->peer);

	if (!(port->ssl = SSL_new(SSL_context)))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not initialize SSL connection: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}
	if (!my_SSL_set_fd(port->ssl, port->sock))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not set SSL socket: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}

aloop:
	/*
	 * Prepare to call SSL_get_error() by clearing thread's OpenSSL error
	 * queue.  In general, the current thread's error queue must be empty
	 * before the TLS/SSL I/O operation is attempted, or SSL_get_error()
	 * will not work reliably.  An extension may have failed to clear the
	 * per-thread error queue following another call to an OpenSSL I/O
	 * routine.
	 */
	ERR_clear_error();
	r = SSL_accept(port->ssl);
	if (r <= 0)
	{
		err = SSL_get_error(port->ssl, r);

		/*
		 * Other clients of OpenSSL in the backend may fail to call
		 * ERR_get_error(), but we always do, so as to not cause problems
		 * for OpenSSL clients that don't call ERR_clear_error()
		 * defensively.  Be sure that this happens by calling now.
		 * SSL_get_error() relies on the OpenSSL per-thread error queue
		 * being intact, so this is the earliest possible point
		 * ERR_get_error() may be called.
		 */
		ecode = ERR_get_error();
		switch (err)
		{
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
#ifdef WIN32
				pgwin32_waitforsinglesocket(SSL_get_fd(port->ssl),
											(err == SSL_ERROR_WANT_READ) ?
						FD_READ | FD_CLOSE | FD_ACCEPT : FD_WRITE | FD_CLOSE,
											INFINITE);
#endif
				goto aloop;
			case SSL_ERROR_SYSCALL:
				if (r < 0)
					ereport(COMMERROR,
							(errcode_for_socket_access(),
							 errmsg("could not accept SSL connection: %m")));
				else
					ereport(COMMERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg("could not accept SSL connection: EOF detected")));
				break;
			case SSL_ERROR_SSL:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("could not accept SSL connection: %s",
								SSLerrmessage(ecode))));
				break;
			case SSL_ERROR_ZERO_RETURN:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
				   errmsg("could not accept SSL connection: EOF detected")));
				break;
			default:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unrecognized SSL error code: %d",
								err)));
				break;
		}
		return -1;
	}

	port->count = 0;

	/* Get client certificate, if available. */
	port->peer = SSL_get_peer_certificate(port->ssl);

	/* and extract the Common Name from it. */
	port->peer_cn = NULL;
	if (port->peer != NULL)
	{
		int			len;

		len = X509_NAME_get_text_by_NID(X509_get_subject_name(port->peer),
										NID_commonName, NULL, 0);
		if (len != -1)
		{
			char	   *peer_cn;

			peer_cn = MemoryContextAlloc(TopMemoryContext, len + 1);
			r = X509_NAME_get_text_by_NID(X509_get_subject_name(port->peer),
										  NID_commonName, peer_cn, len + 1);
			peer_cn[len] = '\0';
			if (r != len)
			{
				/* shouldn't happen */
				pfree(peer_cn);
				return -1;
			}

			/*
			 * Reject embedded NULLs in certificate common name to prevent
			 * attacks like CVE-2009-4034.
			 */
			if (len != strlen(peer_cn))
			{
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("SSL certificate's common name contains embedded null")));
				pfree(peer_cn);
				return -1;
			}

			port->peer_cn = peer_cn;
		}
	}

	ereport(DEBUG2,
			(errmsg("SSL connection from \"%s\"",
					port->peer_cn ? port->peer_cn : "(anonymous)")));

	/* set up debugging/info callback */
	SSL_CTX_set_info_callback(SSL_context, info_cb);

	return 0;
}

/*
 *	Close SSL connection.
 */
static void
close_SSL(Port *port)
{
	if (port->ssl)
	{
		SSL_shutdown(port->ssl);
		SSL_free(port->ssl);
		port->ssl = NULL;
	}

	if (port->peer)
	{
		X509_free(port->peer);
		port->peer = NULL;
	}

	if (port->peer_cn)
	{
		pfree(port->peer_cn);
		port->peer_cn = NULL;
	}
}

/*
 * Obtain reason string for last SSL error
 *
 * ERR_get_error() is used by caller to get errcode to pass here.
 *
 * Some caution is needed here since ERR_reason_error_string will
 * return NULL if it doesn't recognize the error code.  We don't
 * want to return NULL ever.
 */
static const char *
SSLerrmessage(unsigned long ecode)
{
	const char *errreason;
	static char errbuf[ERROR_BUF_SIZE];

	if (ecode == 0)
		return _("no SSL error reported");
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
		return errreason;
	snprintf(errbuf, ERROR_BUF_SIZE, _("SSL error code %lu"), ecode);
	return errbuf;
}

/*
 * Get the server certificate hash for SCRAM channel binding type
 * tls-server-end-point.
 * The result is a palloc'd hash of the server certificate with its
 * size, and NULL if there is no certificate available.
 */
#ifdef HAVE_X509_GET_SIGNATURE_NID
char *
be_tls_get_certificate_hash(Port *port, size_t *len)
{
	X509       *server_cert;
	char       *cert_hash;
	const EVP_MD *algo_type = NULL;
	unsigned char hash[EVP_MAX_MD_SIZE];    /* size for SHA-512 */
	unsigned int hash_size;
	int                     algo_nid;

	*len = 0;
	server_cert = SSL_get_certificate(port->ssl);
	if (server_cert == NULL)
		return NULL;

	/*
	 * Get the signature algorithm of the certificate to determine the
	 * hash algorithm to use for the result.
	 */
	if (!OBJ_find_sigid_algs(X509_get_signature_nid(server_cert),
							 &algo_nid, NULL))
		elog(ERROR, "could not determine server certificate signature algorithm");

	/*
	 * The TLS server's certificate bytes need to be hashed with SHA-256 if
	 * its signature algorithm is MD5 or SHA-1 as per RFC 5929
	 * (https://tools.ietf.org/html/rfc5929#section-4.1).  If something else
	 * is used, the same hash as the signature algorithm is used.
	 */
	switch (algo_nid)
	{
		case NID_md5:
		case NID_sha1:
			algo_type = EVP_sha256();
			break;
		default:
			algo_type = EVP_get_digestbynid(algo_nid);
			if (algo_type == NULL)
				elog(ERROR, "could not find digest for NID %s",
					 OBJ_nid2sn(algo_nid));
			break;
	}

	/* generate and save the certificate hash */
	if (!X509_digest(server_cert, algo_type, hash, &hash_size))
		elog(ERROR, "could not generate server certificate hash");

	cert_hash = palloc(hash_size);
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
}
#endif
#endif   /* USE_SSL */

