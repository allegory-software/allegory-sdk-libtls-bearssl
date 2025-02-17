/* $OpenBSD: tls_client.c,v 1.47 2021/06/01 20:26:11 tb Exp $ */
/*
 * Copyright (c) 2014 Joel Sing <jsing@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#ifdef WIN32
# include <Winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <arpa/inet.h>
# include <netinet/in.h>
# include <netdb.h>
#endif
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <tls.h>
#include "tls_internal.h"

struct tls *
tls_client(void)
{
	struct tls *ctx;

	if (tls_init() == -1)
		return (NULL);

	if ((ctx = tls_new()) == NULL)
		return (NULL);

	ctx->flags |= TLS_CLIENT;

	return (ctx);
}

int
tls_connect(struct tls *ctx, const char *host, const char *port)
{
	return tls_connect_servername(ctx, host, port, NULL);
}

int
tls_connect_servername(struct tls *ctx, const char *host, const char *port,
    const char *servername)
{
	struct addrinfo hints, *res, *res0;
	const char *h = NULL, *p = NULL;
	char *hs = NULL, *ps = NULL;
	int rv = -1, s = -1, ret;

	if ((ctx->flags & TLS_CLIENT) == 0) {
		tls_set_errorx(ctx, "not a client context");
		goto err;
	}

	if (host == NULL) {
		tls_set_errorx(ctx, "host not specified");
		goto err;
	}

	/*
	 * If port is NULL try to extract a port from the specified host,
	 * otherwise use the default.
	 */
	if ((p = (char *)port) == NULL) {
		ret = tls_host_port(host, &hs, &ps);
		if (ret == -1) {
			tls_set_errorx(ctx, "memory allocation failure");
			goto err;
		}
		if (ret != 0) {
			tls_set_errorx(ctx, "no port provided");
			goto err;
		}
	}

	h = (hs != NULL) ? hs : host;
	p = (ps != NULL) ? ps : port;

	/*
	 * First check if the host is specified as a numeric IP address,
	 * either IPv4 or IPv6, before trying to resolve the host.
	 * The AI_ADDRCONFIG resolver option will not return IPv4 or IPv6
	 * records if it is not configured on an interface;  not considering
	 * loopback addresses.  Checking the numeric addresses first makes
	 * sure that connection attempts to numeric addresses and especially
	 * 127.0.0.1 or ::1 loopback addresses are always possible.
	 */
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;

	/* try as an IPv4 literal */
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_NUMERICHOST;
	if (getaddrinfo(h, p, &hints, &res0) != 0) {
		/* try again as an IPv6 literal */
		hints.ai_family = AF_INET6;
		if (getaddrinfo(h, p, &hints, &res0) != 0) {
			/* last try, with name resolution and save the error */
			hints.ai_family = AF_UNSPEC;
			hints.ai_flags = AI_ADDRCONFIG;
			if ((s = getaddrinfo(h, p, &hints, &res0)) != 0) {
				tls_set_errorx(ctx, "%s", gai_strerror(s));
				goto err;
			}
		}
	}

	/* It was resolved somehow; now try connecting to what we got */
	s = -1;
	for (res = res0; res; res = res->ai_next) {
		s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == -1) {
			tls_set_error(ctx, "socket");
			continue;
		}
		if (connect(s, res->ai_addr, res->ai_addrlen) == -1) {
			tls_set_error(ctx, "connect");
			close(s);
			s = -1;
			continue;
		}

		break;  /* Connected. */
	}
	freeaddrinfo(res0);

	if (s == -1)
		goto err;

	if (servername == NULL)
		servername = h;

	if (tls_connect_socket(ctx, s, servername) != 0) {
		close(s);
		goto err;
	}

	ctx->socket = s;

	rv = 0;

 err:
	free(hs);
	free(ps);

	return (rv);
}

static int
tls_connect_common(struct tls *ctx, const char *servername)
{
	union tls_addr addrbuf;
	struct tls_keypair *kp;
	size_t servername_len;
	int rv = -1;

	if ((ctx->flags & TLS_CLIENT) == 0) {
		tls_set_errorx(ctx, "not a client context");
		goto err;
	}

	if (servername != NULL) {
		if ((ctx->servername = strdup(servername)) == NULL) {
			tls_set_errorx(ctx, "out of memory");
			goto err;
		}

		/*
		 * If there's a trailing dot, remove it. While an FQDN includes
		 * the terminating dot representing the zero-length label of
		 * the root (RFC 8499, section 2), the SNI explicitly does not
		 * include it (RFC 6066, section 3).
		 */
		servername_len = strlen(ctx->servername);
		if (servername_len > 0 &&
		    ctx->servername[servername_len - 1] == '.')
			ctx->servername[servername_len - 1] = '\0';

		/*
		 * RFC 6066 (SNI): Literal IPv4 and IPv6 addresses are not
		 * permitted in "HostName".
		 */
		if (inet_pton(AF_INET, ctx->servername, &addrbuf) != 1 &&
		    inet_pton(AF_INET6, ctx->servername, &addrbuf) != 1) {
			servername = ctx->servername;
		} else {
			servername = NULL;
		}
	}

	if (ctx->config->ocsp_require_stapling != 0) {
		tls_set_errorx(ctx, "OCSP stapling is not supported");
		goto err;
	}

	if ((ctx->conn = tls_conn_new(ctx)) == NULL)
		goto err;

	if (tls_configure_x509(ctx) != 0)
		goto err;

	br_ssl_client_set_default_rsapub(&ctx->conn->u.client);

	kp = ctx->config->keypair;
	if (kp) {
		switch (kp->key_type) {
		case BR_KEYTYPE_RSA:
			br_ssl_client_set_single_rsa(&ctx->conn->u.client,
			    kp->chain, kp->chain_len, &kp->key.rsa,
			    br_rsa_pkcs1_sign_get_default());
			break;
		case BR_KEYTYPE_EC:
			/* BR_KEYTYPE_KEYX is only used for ECDH, which
			 * is not supported by libtls */
			br_ssl_client_set_single_ec(&ctx->conn->u.client,
			    kp->chain, kp->chain_len, &kp->key.ec, BR_KEYTYPE_SIGN,
			    kp->signer_key_type, br_ec_get_default(),
			    br_ecdsa_sign_asn1_get_default());
			break;
		}
	}

	if (ctx->config->verify_name) {
		if (servername == NULL) {
			tls_set_errorx(ctx, "server name not specified");
			goto err;
		}
	}

	br_ssl_client_reset(&ctx->conn->u.client, servername, 0);

	ctx->state |= TLS_CONNECTED;
	rv = 0;

 err:
	return (rv);
}

int
tls_connect_socket(struct tls *ctx, int s, const char *servername)
{
	return tls_connect_fds(ctx, s, s, servername);
}

int
tls_connect_fds(struct tls *ctx, int fd_read, int fd_write,
    const char *servername)
{
	int rv = -1;

	if (fd_read < 0 || fd_write < 0) {
		tls_set_errorx(ctx, "invalid file descriptors");
		goto err;
	}

	if (tls_connect_common(ctx, servername) != 0)
		goto err;

	ctx->fd_read = fd_read;
	ctx->read_cb = tls_fd_read_cb;
	ctx->fd_write = fd_write;
	ctx->write_cb = tls_fd_write_cb;
	ctx->cb_arg = NULL;

	rv = 0;
 err:
	return (rv);
}

int
tls_connect_cbs(struct tls *ctx, tls_read_cb read_cb,
    tls_write_cb write_cb, void *cb_arg, const char *servername)
{
	int rv = -1;

	if (tls_connect_common(ctx, servername) != 0)
		goto err;

	if (read_cb == NULL || write_cb == NULL) {
		tls_set_errorx(ctx, "no callbacks provided");
		goto err;
	}
	ctx->read_cb = read_cb;
	ctx->write_cb = write_cb;
	ctx->cb_arg = cb_arg;

	rv = 0;

 err:
	return (rv);
}
