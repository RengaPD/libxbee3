/*
	libxbee - a C library to aid the use of Digi's XBee wireless modules
	          running in API mode (AP=2).

	Copyright (C) 2009	Attie Grande (attie@attie.co.uk)

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.	If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "internal.h"
#include "xbee_int.h"
#include "rx.h"
#include "tx.h"
#include "frame.h"
#include "conn.h"
#include "net.h"
#include "net_handlers.h"
#include "net_callbacks.h"
#include "mode.h"
#include "log.h"
#include "ll.h"
#include "thread.h"

struct ll_head *netDeadClientList = NULL;

/* ######################################################################### */

xbee_err xbee_netRx(struct xbee *xbee, void *arg, struct xbee_buf **buf) {
	struct xbee_netClientInfo *info;
	char c;
	char length[2];
	int pos, len, ret;
	struct xbee_buf *iBuf;
	int fd;
	
	if (!xbee || !buf || !arg) return XBEE_EMISSINGPARAM;
	
	info = arg;
	if (xbee != info->xbee) return XBEE_EINVAL;
	fd = info->fd;
	
	while (1) {
		do {
			if ((ret = recv(fd, &c, 1, MSG_NOSIGNAL)) < 0) return XBEE_EIO;
			if (ret == 0) goto eof;
		} while (c != 0x7E);
		
		for (len = 2, pos = 0; pos < len; pos += ret) {
			ret = recv(fd, &(length[pos]), len - pos, MSG_NOSIGNAL);
			if (ret > 0) continue;
			if (ret == 0) goto eof;
			return XBEE_EIO;
		}
		
		if ((iBuf = malloc(sizeof(*iBuf) + len)) == NULL) return XBEE_ENOMEM;
		ll_add_tail(needsFree, iBuf);
		
		iBuf->len = ((length[0] << 8) & 0xFF00) || (length[1] & 0xFF);
		
		for (pos = 0; pos < iBuf->len; pos += ret) {
			ret = recv(fd, &(iBuf->data[pos]), iBuf->len - pos, MSG_NOSIGNAL);
			if (ret > 0) continue;
			ll_ext_item(needsFree, iBuf);
			free(iBuf);
			if (ret == 0) goto eof;
			return XBEE_EIO;
		}
	}
	
	*buf = iBuf;
	
	return XBEE_ENONE;
eof:
	/* xbee_netRx() is responsible for free()ing memory and killing off client threads on the server
	   to do this, we need to add ourselves to the netDeadClientList, and remove ourselves from the clientList
	   the server thread will then cleanup any clients on the next accept() */
	ll_add_tail(netDeadClientList, info);
	ll_ext_item(xbee->netInfo->clientList, info);
	return XBEE_EEOF;
}

xbee_err xbee_netTx(struct xbee *xbee, void *arg, struct xbee_buf *buf) {
	struct xbee_netClientInfo *info;
	int pos, ret;
	int fd;
	
	if (!xbee || !buf || !arg) return XBEE_EMISSINGPARAM;
	
	info = arg;
	if (xbee != info->xbee) return XBEE_EINVAL;
	fd = info->fd;
	
	for (pos = 0; pos < buf->len; pos += ret) {
		ret = send(fd, buf->data, buf->len - pos, MSG_NOSIGNAL);
		if (ret >= 0) continue;
		return XBEE_EIO;
	}
	
	return XBEE_ENOTIMPLEMENTED;
}

/* ######################################################################### */

xbee_err xbee_netClientAlloc(struct xbee *xbee, struct xbee_netClientInfo **info) {
	xbee_err ret;
	struct xbee_netClientInfo *iInfo;
	
	if (!info) return XBEE_EMISSINGPARAM;
	
	if ((iInfo = malloc(sizeof(*iInfo))) == NULL) return XBEE_ENOMEM;
	*info = iInfo;

	ret = XBEE_ENONE;
	
	if ((ret = xbee_rxAlloc(&iInfo->iface.rx)) != XBEE_ENONE) goto die;
	if ((ret = xbee_txAlloc(&iInfo->iface.tx)) != XBEE_ENONE) goto die;
	if ((ret = xbee_frameBlockAlloc(&iInfo->fBlock)) != XBEE_ENONE) goto die;
	
	iInfo->xbee = xbee;
	
	iInfo->iface.rx->handlerArg = iInfo;
	iInfo->iface.rx->ioArg = iInfo;
	iInfo->iface.rx->ioFunc = xbee_netRx;
	iInfo->iface.rx->fBlock = iInfo->fBlock;
#define xbee_netConTypes NULL
	iInfo->iface.rx->conTypes = xbee_netConTypes;
	
	iInfo->iface.tx->ioArg = iInfo;
	iInfo->iface.tx->ioFunc = xbee_netTx;
	
	goto done;
die:
	*info = NULL;
	xbee_netClientFree(iInfo);
done:
	return ret;
}

xbee_err xbee_netClientFree(struct xbee_netClientInfo *info) {
	if (!info) return XBEE_EINVAL;
	
	xbee_frameBlockFree(info->fBlock);
	xbee_txFree(info->iface.tx);
	xbee_rxFree(info->iface.rx);
	
	free(info);
	return XBEE_ENONE;
}

/* ######################################################################### */

xbee_err xbee_netConNew(struct xbee *xbee, struct xbee_netClientInfo *client, char *type, unsigned char endpoint, xbee_t_conCallback callback) {
	xbee_err ret;
	struct xbee_conAddress address;
	struct xbee_con *con;
	
	if (!xbee || !client || !type || !callback) return XBEE_EMISSINGPARAM;
	
	memset(&address, 0, sizeof(address));
	address.endpoints_enabled = 1;
	address.endpoint_local = endpoint;
	address.endpoint_remote = endpoint;
	
	if ((ret = _xbee_conNew(xbee, &client->iface, &con, type, &address)) != XBEE_ENONE) return ret;
	if (!con) return XBEE_EUNKNOWN;
	
	xbee_conDataSet(con, client, NULL);
	xbee_conCallbackSet(con, callback, NULL);
	
	return XBEE_ENONE;
}

xbee_err xbee_netClientSetupBackchannel(struct xbee *xbee, struct xbee_netClientInfo *client) {
	xbee_err ret;
	int endpoint;

	if (!xbee || !client) return XBEE_EMISSINGPARAM;
	
	for (endpoint = 0; xbee_netServerCallbacks[endpoint].callback; endpoint++) {
		if ((ret = xbee_netConNew(xbee, client, "Backchannel", endpoint, xbee_netServerCallbacks[endpoint].callback)) != XBEE_ENONE) return ret;
	}
	
	/* 'frontchannel' connections are added later... by the developer... you know, with xbee_conNew()... */
	
	return ret;
}

/* ######################################################################### */

xbee_err xbee_netClientStartup(struct xbee *xbee, struct xbee_netClientInfo *client) {
	xbee_err ret;
	
	if (!xbee || !client) return XBEE_EMISSINGPARAM;
	
	ret = XBEE_ENONE;
	
	if ((ret = xbee_netClientSetupBackchannel(xbee, client)) != XBEE_ENONE) return ret;
	
	if ((ret = xbee_threadStart(xbee, &client->rxThread, 150000, xbee_rx, client->iface.rx)) != XBEE_ENONE) {
		xbee_log(1, "failed to start xbee_rx() thread for client from %s:%d", client->addr, client->port);
		ret = XBEE_ETHREAD;
		goto die1;
	}
	if ((ret = xbee_threadStart(xbee, &client->rxHandlerThread, 150000, xbee_rxHandler, client->iface.rx)) != XBEE_ENONE) {
		xbee_log(1, "failed to start xbee_rx() thread for client from %s:%d", client->addr, client->port);
		ret = XBEE_ETHREAD;
		goto die2;
	}
	if ((ret = xbee_threadStart(xbee, &client->txThread, 150000, xbee_tx, client->iface.tx)) != XBEE_ENONE) {
		xbee_log(1, "failed to start xbee_tx() thread for client from %s:%d", client->addr, client->port);
		ret = XBEE_ETHREAD;
		goto die3;
	}
	return XBEE_ENONE;
die3:
	xbee_threadKillJoin(xbee, client->rxHandlerThread, NULL);
die2:
	xbee_threadKillJoin(xbee, client->rxThread, NULL);
die1:
	return ret;
}

xbee_err xbee_netClientShutdown(struct xbee_netClientInfo *client) {
	if (!client) return XBEE_EMISSINGPARAM;
	if (!client->xbee) return XBEE_EINVAL;

	xbee_threadKillJoin(client->xbee, client->txThread, NULL);
	xbee_threadKillJoin(client->xbee, client->rxHandlerThread, NULL);
	xbee_threadKillJoin(client->xbee, client->rxThread, NULL);
	
	shutdown(client->fd, SHUT_RDWR);
	xsys_close(client->fd);
	
	xbee_netClientFree(client);
	
	return XBEE_ENONE;
}

/* ######################################################################### */

xbee_err xbee_netServerThread(struct xbee *xbee, int *restart, void *arg) {
	xbee_err ret;
	struct sockaddr_in addrinfo;
	socklen_t addrlen;
	char addr[INET_ADDRSTRLEN];
	int port;
	
	struct xbee_netInfo *info;
	struct xbee_netClientInfo *client;
	struct xbee_netClientInfo *deadClient;
	
	if (!xbee->netInfo || arg != xbee->netInfo) {
		*restart = 0;
		return XBEE_EINVAL;
	}
	
	client = NULL;
	while (xbee->netInfo) {
		ret = XBEE_ENONE;
		info = xbee->netInfo;
		
		while (ll_ext_head(netDeadClientList, (void**)&deadClient) == XBEE_ENONE && deadClient != NULL) {
			xbee_netClientShutdown(deadClient);
		}
		
		if (!client) {
			if ((ret = xbee_netClientAlloc(xbee, &info->newClient)) != XBEE_ENONE) return ret;
			client = info->newClient;
			client->xbee = xbee;
		}
		
		addrlen = sizeof(addrinfo);
		if ((client->fd = accept(info->fd, (struct sockaddr*)&addrinfo, &addrlen)) < 0) {
			ret = XBEE_EIO;
			if (errno == EINVAL) {
				/* it appears we aren't listening yet... or have stopped listening. let's sleep for 5ms and try again */
				usleep(5000);
				continue;
			}
			break;
		}
		
		if (!xbee->netInfo) {
			shutdown(client->fd, SHUT_RDWR);
			close(client->fd);
			break;
		}
		
		memset(addr, 0, sizeof(addr));
		if (inet_ntop(AF_INET, (const void*)&addrinfo.sin_addr, addr, sizeof(addr)) == NULL) {
			shutdown(client->fd, SHUT_RDWR);
			close(client->fd);
			ret = XBEE_EIO;
			break;
		}
		port = ntohs(addrinfo.sin_port);
		
		if (info->clientFilter) {
			if (info->clientFilter(xbee, addr) != 0) {
				shutdown(client->fd, SHUT_RDWR);
				close(client->fd);
				xbee_log(1, "*** connection from %s:%d was blocked ***", addr, port);
				continue;
			}
		}
		
		memcpy(client->addr, addr, sizeof(client->addr));
		client->port = port;
		
		if ((ret = xbee_modeImport(&client->iface.conTypes, &xbee_netServerMode)) != XBEE_ENONE) {
			shutdown(client->fd, SHUT_RDWR);
			close(client->fd);
			xbee_log(10, "failed to accept client... xbee_modeImport() returned %d", ret);
			continue;
		}
		if ((ret = xbee_netClientStartup(xbee, client)) != XBEE_ENONE) {
			shutdown(client->fd, SHUT_RDWR);
			close(client->fd);
			xbee_log(10, "failed to accept client... xbee_netClientStartup() returned %d", ret);
			continue;
		}
		
		xbee_log(10, "accepted connection from %s:%d", addr, port);
		
		ll_add_tail(info->clientList, client);
		info->newClient = NULL;
		client = NULL;
	}
	
	if (xbee->netInfo) xbee->netInfo->newClient = NULL;
	if (client) {	
		xbee_netClientFree(client);
	}
	
	return ret;
}

/* ######################################################################### */

EXPORT xbee_err xbee_netStart(struct xbee *xbee, int port, int(*clientFilter)(struct xbee *xbee, char *remoteHost)) {
	xbee_err ret;
	int fd;
	int i;
  struct sockaddr_in addrinfo;
	
	if (!xbee) return XBEE_EMISSINGPARAM;
	if (xbee->netInfo != NULL) return XBEE_EINVAL;
	if (port <= 0 || port >= 65535) return XBEE_EINVAL;
	
	if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) return XBEE_EIO;
	
	i = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) == -1) {
		xsys_close(fd);
		return XBEE_EIO;
	}
	
	memset(&addrinfo, 0, sizeof(addrinfo));
	addrinfo.sin_family = AF_INET;
	addrinfo.sin_port = htons(port);
	addrinfo.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(fd, (const struct sockaddr*)&addrinfo, sizeof(addrinfo)) == -1) {
		xsys_close(fd);
		return XBEE_EIO;
	}
	
	if ((ret = xbee_netvStart(xbee, fd, clientFilter)) != XBEE_ENONE) {
		xsys_close(fd);
	}
	
	return ret;
}

EXPORT xbee_err xbee_netvStart(struct xbee *xbee, int fd, int(*clientFilter)(struct xbee *xbee, char *remoteHost)) {
	xbee_err ret;
	struct xbee_netInfo *info;
	
	if (!xbee) return XBEE_EMISSINGPARAM;
	if (fd < 0 || xbee->netInfo != NULL) return XBEE_EINVAL;
	
	ret = XBEE_ENONE;
	
	if ((info = malloc(sizeof(*info))) == NULL) return XBEE_ENOMEM;
	memset(info, 0, sizeof(*info));
	
	if ((info->clientList = ll_alloc()) == NULL) {
		free(info);
		return XBEE_ENOMEM;
	}
	
	info->fd = fd;
	info->clientFilter = clientFilter;
	
	xbee->netInfo = info;
	
	if ((ret = xbee_threadStart(xbee, &info->serverThread, 150000, xbee_netServerThread, info)) == XBEE_ENONE) {
		if (listen(fd, 512) == -1) return XBEE_EIO;
	}
	
	return ret;
}

/* ######################################################################### */

EXPORT xbee_err xbee_netStop(struct xbee *xbee) {
	struct xbee_netInfo *info;
	struct xbee_netClientInfo *deadClient;
	
	if (!xbee) return XBEE_EMISSINGPARAM;
	if (!xbee->netInfo) return XBEE_EINVAL;
	
	info = xbee->netInfo;
	xbee->netInfo = NULL;
	
	/* closing the listening fd, will cause the accept() in the serverThread to return an error.
	   once that returns, the while() loop will break, and the thread will die - no need for threadKillRelease() */
	xbee_threadStopRelease(xbee, info->serverThread);
	shutdown(info->fd, SHUT_RDWR);
	xsys_close(info->fd);
	
	ll_free(info->clientList, (void(*)(void*))xbee_netClientShutdown);
	
	while (ll_ext_head(netDeadClientList, (void**)&deadClient) == XBEE_ENONE && deadClient != NULL) {
		xbee_netClientShutdown(deadClient);
	}
	
	free(info);

	return XBEE_ENONE;
}
