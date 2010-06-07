/*
  +----------------------------------------------------------------------+
  | PHP Version 5														|
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2007 The PHP Group								|
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,	  |
  | that is bundled with this package in the file LICENSE, and is		|
  | available through the world-wide-web at the following url:		   |
  | http://www.php.net/license/3_01.txt								  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to		  |
  | license@php.net so we can mail you a copy immediately.			   |
  |																	  |
  | This source uses the libamqpmq under the MPL. For the MPL, please	|
  | see LICENSE-MPL-RabbitMQ											 |
  +----------------------------------------------------------------------+
  | Author: Alexandre Kalendarev akalend@mail.ru Copyright (c) 2009-2010 |
  | Contributor: Pieter de Zwart pdezwart@php.net						|
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

#include "php_amqp.h"

#include <stdint.h>
#include <signal.h>
#include <amqp.h>
#include <amqp_framing.h>

#include <unistd.h>

#define FRAME_MAX				131072  /* max length (size) of frame */
#define HEADER_FOOTER_SIZE		8	   /*  7 bytes up front, then payload, then 1 byte footer */
#define PORT					5672	/* default AMQP port */
#define PORT_STR				"5672"
#define AMQP_CHANNEL			1	   /* default channal number */
#define AMQP_HEARBEAT			0	   /* hearbeat */

#define AMQP_NULLARGS			amqp_table_t arguments = {0, NULL};
#define AMQP_PASSIVE_D			short passive = (AMQP_PASSIVE & parms) ? 1 : 0;
#define AMQP_DURABLE_D			short durable = (AMQP_DURABLE & parms) ? 1 : 0;
#define AMQP_AUTODELETE_D		short auto_delete = (AMQP_AUTODELETE & parms) ? 1 : 0;
#define AMQP_EXCLUSIVE_D		short exclusive = (AMQP_EXCLUSIVE & parms) ? 1 : 0;

#define AMQP_SET_NAME(ctx, str) (ctx)->name_len = strlen(str) >= sizeof((ctx)->name) ? sizeof((ctx)->name) - 1 : strlen(str); \
								strncpy((ctx)->name, name, (ctx)->name_len); \
								(ctx)->name[(ctx)->name_len] = '\0';
										
/* If you declare any globals in php_amqp.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(amqp)
*/

/* True global resources - no need for thread safety here */
zend_class_entry *amqp_class_entry;
zend_class_entry *amqp_queue_class_entry;
zend_class_entry *amqp_exchange_class_entry;
zend_class_entry *amqp_exception_class_entry,
				 *amqp_connection_exception_class_entry,
				 *amqp_exchange_exception_class_entry,
				 *amqp_queue_exception_class_entry;

typedef struct _amqp_object {
	zend_object zo;
	char is_connected;
	char is_channel_connected;
	int fd;
	amqp_connection_state_t conn;
} amqp_object;

typedef struct _amqp_queue_object {
	zend_object zo;
	zval *cnn;
	char is_connected;
	char name[64];
	int name_len;
	char consumer_tag[64];
	int consumer_tag_len;
} amqp_queue_object;


typedef struct _amqp_exchange_object {
	zend_object zo;
	zval *cnn;
	char is_connected;
	char name[64];
	int name_len;
} amqp_exchange_object;


/* amqp_class ARG_INFO definition */
ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_class__construct, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_ARRAY_INFO(0, credentials, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_class_isConnected, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO()

/* amqp_queue_class ARG_INFO definition */
ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class__construct, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, amqp_connection)
	ZEND_ARG_INFO(0, queue_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_declare, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, queue_name)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_delete, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, queue_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_purge, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, queue_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_bind, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, exchange_name)
	ZEND_ARG_INFO(0, routing_key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_unbind, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, exchange_name)
	ZEND_ARG_INFO(0, routing_key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_consume, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, num_messages)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_get, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_cancel, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, consumer_tag)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_queue_class_ack, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, delivery_tag)
ZEND_END_ARG_INFO()

/* amqp_exchange ARG_INFO definition */
ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_exchange_class__construct, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, amqp_connection)
	ZEND_ARG_INFO(0, exchange_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_exchange_class_declare, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, exchange_name)
	ZEND_ARG_INFO(0, exchange_type)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_exchange_class_bind, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, exchange_name)
	ZEND_ARG_INFO(0, routing_key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_exchange_class_delete, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, exchange_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_amqp_exchange_class_publish, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 2)
	ZEND_ARG_INFO(0, message)
	ZEND_ARG_INFO(0, routing_key)
ZEND_END_ARG_INFO()


/* {{{ amqp_functions[]
*
*Every user visible function must have an entry in amqp_functions[].
*/
zend_function_entry amqp_class_functions[] = {
	PHP_ME(amqp_class, __construct, arginfo_amqp_class__construct,  ZEND_ACC_PUBLIC)
	PHP_ME(amqp_class, isConnected, arginfo_amqp_class_isConnected, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}	/* Must be the last line in amqp_functions[] */
};

zend_function_entry amqp_queue_class_functions[] = {
	PHP_ME(amqp_queue_class, __construct,	arginfo_amqp_queue_class__construct,	ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, declare,		arginfo_amqp_queue_class_declare,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, delete,		arginfo_amqp_queue_class_delete,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, purge,			arginfo_amqp_queue_class_purge,			ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, bind,			arginfo_amqp_queue_class_bind,			ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, unbind,		arginfo_amqp_queue_class_unbind,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, consume,		arginfo_amqp_queue_class_consume,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, get,			arginfo_amqp_queue_class_get,			ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, cancel,		arginfo_amqp_queue_class_cancel,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_queue_class, ack,			arginfo_amqp_queue_class_ack,			ZEND_ACC_PUBLIC)
	
	{NULL, NULL, NULL}	/* Must be the last line in amqp_functions[] */
};


zend_function_entry amqp_exchange_class_functions[] = {
	PHP_ME(amqp_exchange_class, __construct,	arginfo_amqp_exchange_class__construct, ZEND_ACC_PUBLIC)
	PHP_ME(amqp_exchange_class, declare,		arginfo_amqp_exchange_class_declare,	ZEND_ACC_PUBLIC)
	PHP_ME(amqp_exchange_class, bind,			arginfo_amqp_exchange_class_bind,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_exchange_class, delete,			arginfo_amqp_exchange_class_delete,		ZEND_ACC_PUBLIC)
	PHP_ME(amqp_exchange_class, publish,		arginfo_amqp_exchange_class_publish,	ZEND_ACC_PUBLIC)

	/* PHP_ME(amqp_queue_class, unbind,		 NULL, ZEND_ACC_PUBLIC) */

	{NULL, NULL, NULL}	/* Must be the last line in amqp_functions[] */
}; 

zend_function_entry amqp_functions[] = {
	{NULL, NULL, NULL}	/* Must be the last line in amqp_functions[] */
};
/* }}} */

/* {{{ amqp_module_entry
*/
zend_module_entry amqp_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"amqp",
	amqp_functions,
	PHP_MINIT(amqp),
	PHP_MSHUTDOWN(amqp),
	NULL,	
	NULL,	
	PHP_MINFO(amqp),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1",
#endif
	STANDARD_MODULE_PROPERTIES
};
	/* }}} */

#ifdef COMPILE_DL_AMQP
	ZEND_GET_MODULE(amqp)
#endif


static void  amqp_error(amqp_rpc_reply_t x, char ** pstr) {
	switch (x.reply_type) {
		case AMQP_RESPONSE_NORMAL:
		return;

		case AMQP_RESPONSE_NONE:
		spprintf(pstr, 0, "Missing RPC reply type.");
		break;

		case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		spprintf(pstr, 0, "Library error: %s\n",strerror(x.library_errno));
		break;

		case AMQP_RESPONSE_SERVER_EXCEPTION:
		switch (x.reply.id) {
			case AMQP_CONNECTION_CLOSE_METHOD: {
				amqp_connection_close_t *m = (amqp_connection_close_t *)x.reply.decoded;

				spprintf(pstr, 0, "Server connection error: %d, message: %.*s",
					m->reply_code,
					(int) m->reply_text.len,
					(char *)m->reply_text.bytes);
				break;
			}
			case AMQP_CHANNEL_CLOSE_METHOD: {
				amqp_channel_close_t *m = (amqp_channel_close_t *) x.reply.decoded;
				spprintf(pstr, 0, "Server channel error: %d, message: %.*s",
					m->reply_code,
					(int)m->reply_text.len,
					(char *)m->reply_text.bytes);
				break;
			}
			default:
			spprintf(pstr, 0, "Unknown server error, method id 0x%08X",  x.reply.id);
			break;
		}
		break;
	}
}


/* {{{ proto amqp::__construct(string string host=localhost, int port=PORT, login=quest, string psw=quest)
amqp constructor */
PHP_METHOD(amqp_class, __construct)
{
	zval *id;
	amqp_object *ctx;

	zval* iniArr = NULL;
	zval** zdata;

	char *login;
	char *psw;
	char *host;
	char *vhost;
	char str[256] ;
	char ** pstr = (char **) &str;
	int port = 0;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|a", &id, amqp_class_entry, &iniArr) == FAILURE) {
		php_std_error_handling();
		return;
	}
	php_std_error_handling();

	ctx = (amqp_object *)zend_object_store_get_object(id TSRMLS_CC);
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "login", sizeof("login"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0) {
		if (Z_STRLEN_PP(zdata) < 32) {
			login = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'login' exceeds 32 characters limit.", 0 TSRMLS_CC);
			return;
		}		 
	} else { 
		login = estrndup(INI_STR("amqp.login"), strlen(INI_STR("amqp.login")) > 32 ? 32 : strlen(INI_STR("amqp.login")));
	}

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF(iniArr), "password", sizeof("password"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0) {
		if (Z_STRLEN_PP(zdata) < 32) {
			psw = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'password' exceeds 32 characters limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		psw = estrndup(INI_STR("amqp.password"), strlen(INI_STR("amqp.password")) > 32 ? 32 : strlen(INI_STR("amqp.password")));
	}

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF(iniArr), "host", sizeof("host"), (void *)&zdata) && strlen(Z_STRVAL_PP(zdata)) > 0) {
		if (Z_STRLEN_PP(zdata) < 32) {
			host = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'host' exceeds 32 characters limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		host = estrndup(INI_STR("amqp.host"), strlen(INI_STR("amqp.host")) > 32 ? 32 : strlen(INI_STR("amqp.host")));
	}

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "vhost", sizeof("vhost"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0) {
		if (Z_STRLEN_PP(zdata) < 32) {
			vhost = estrndup(Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
		} else {
			zend_throw_exception(amqp_connection_exception_class_entry, "Parameter 'vhost' exceeds 32 characters limit.", 0 TSRMLS_CC);
			return;
		}
	} else {
		vhost = estrndup(INI_STR("amqp.vhost"), strlen(INI_STR("amqp.vhost")) > 32 ? 32 : strlen(INI_STR("amqp.vhost")));
	}

	port = INI_INT("amqp.port");

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "port", sizeof("port"), (void*)&zdata)) {
		port = (size_t)Z_LVAL_PP(zdata);
	}

	ctx->conn = amqp_new_connection();

	ctx->fd = amqp_open_socket(host, port);
	
	if (ctx->fd < 1) {
		efree(host);
		efree(psw);
		efree(login);
		efree(vhost);
		
		amqp_destroy_connection(ctx->conn);	
		zend_throw_exception(amqp_connection_exception_class_entry, "Socket error: could not connect to host.", 0 TSRMLS_CC);
		return;
	}
	ctx->is_connected = '\1';

	amqp_set_sockfd(ctx->conn, ctx->fd);

	amqp_rpc_reply_t x = amqp_login(ctx->conn, vhost, 0, FRAME_MAX, AMQP_HEARBEAT, AMQP_SASL_METHOD_PLAIN, login, psw);

	efree(host);
	efree(psw);
	efree(login);
	efree(vhost);

	if (x.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_error(x, pstr); 
		zend_throw_exception(amqp_connection_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	amqp_channel_open(ctx->conn, AMQP_CHANNEL);

	x = amqp_get_rpc_reply(ctx->conn);
	if (x.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_error(x, pstr);
		zend_throw_exception(amqp_connection_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	ctx->is_channel_connected = '\1';
}
/* }}} */


/* {{{ proto amqp::isConnected()
check amqp connection */
PHP_METHOD(amqp_class, isConnected)
{
	zval *id;
	amqp_object *ctx;

	/* Try to pull amqp object out of method params */
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O", &id, amqp_class_entry) == FAILURE) {
		php_std_error_handling();
		return;
	}

	/* Get the connection object out of the store */
	ctx = (amqp_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* If the channal_connect is 1, we have a connection */
	if (ctx->is_channel_connected == '\1') {
		RETURN_TRUE;
	}

	/* We have no connection */
	RETURN_FALSE;
}

/* }}} */
/* ------------------  class Queue ----------- */


/* {{{ proto AMQPQueue::__construct(AMQPConnection cnn,  [string name])
AMQPQueue constructor 
*/
PHP_METHOD(amqp_queue_class, __construct)
{
	zval *id;
	zval* cnnOb = NULL;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name = NULL;
	int name_len = 0; 

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oo|s", 
	&id, amqp_queue_class_entry, &cnnOb, &name, &name_len) == FAILURE) {
		php_std_error_handling();
		return;
	}

	php_std_error_handling();
	if (!instanceof_function(Z_OBJCE_P(cnnOb), amqp_class_entry TSRMLS_CC)) {
		zend_throw_exception(amqp_queue_exception_class_entry, "The first parameter must be and instance of AMQPConnection.", 0 TSRMLS_CC);
		return;
	} 

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* We have a valid connection: */
	ctx->is_connected = '\1';
	ctx->cnn = cnnOb;

	if (name_len) {
		AMQP_SET_NAME(ctx, name);
	}

	ctx_cnn = (amqp_object *)zend_object_store_get_object(cnnOb TSRMLS_CC);
}
/* }}} */


/* {{{ proto int AMQPQueue::declare(string queueName,[ bit params=AMQP_AUTODELETE ]);
declare queue 
*/
PHP_METHOD(amqp_queue_class, declare)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len = 0;
	long parms = 0;
	amqp_queue_declare_t s;

	amqp_rpc_reply_t res;

	amqp_queue_declare_ok_t *r;
	amqp_basic_properties_t props;
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|sl", &id,
	amqp_queue_class_entry, &name, &name_len, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not declare queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);
	if (!parms) {
		parms = AMQP_AUTODELETE; /* default settings */
	}

	amqp_bytes_t amqp_name;
	if (name_len) {
		AMQP_SET_NAME(ctx, name);
		amqp_name = (amqp_bytes_t) {name_len, name};
	} else {
		amqp_name = (amqp_bytes_t) {ctx->name_len, ctx->name};
	}

	AMQP_EXCLUSIVE_D
	AMQP_NULLARGS
	AMQP_PASSIVE_D
	AMQP_DURABLE_D
	AMQP_AUTODELETE_D

	res = AMQP_SIMPLE_RPC(cnn->conn, 
		AMQP_CHANNEL,
		QUEUE,
		DECLARE,
		DECLARE_OK,
		amqp_queue_declare_t,
		0,
		amqp_name,
		passive,
		durable,
		exclusive,
		auto_delete,
		0,
		arguments
	);

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 
		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	r = (amqp_queue_declare_ok_t *) res.reply.decoded;

	RETURN_LONG(r->message_count);
}
/* }}} */


/* {{{ proto int queue::delete(name);
delete queue 
*/
PHP_METHOD(amqp_queue_class, delete)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len = 0;
	long parms = 0;

	amqp_rpc_reply_t res;
	amqp_rpc_reply_t result;
	amqp_queue_delete_ok_t *r;
	amqp_queue_delete_t s; 

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|sl", &id,
	amqp_queue_class_entry, &name, &name_len, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not delete queue. No connection available.", 0 TSRMLS_CC);
		return;
	}
	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	if (!cnn || !cnn->conn) {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not delete queue. The connection is closed.", 0 TSRMLS_CC);
		return;
	}

	if (name_len) {
		s.ticket		= 0;
		s.queue.len	 = name_len;
		s.queue.bytes   = name;
		s.if_unused	 = (AMQP_IFUNUSED & parms)? 1:0;
		s.if_empty	  = (AMQP_IFEMPTY & parms)? 1:0;
		s.nowait		= 0;
	} else {
		s.ticket		= 0;
		s.queue.len	 = ctx->name_len;
		s.queue.bytes   = ctx->name;
		s.if_unused	 = (AMQP_IFUNUSED & parms) ? 1 : 0;
		s.if_empty	  = (AMQP_IFEMPTY & parms) ? 1 : 0;
		s.nowait		= 0;
	}

	amqp_method_number_t method_ok = AMQP_QUEUE_DELETE_OK_METHOD;

	result = amqp_simple_rpc(cnn->conn, 
		AMQP_CHANNEL, 
		AMQP_QUEUE_DELETE_METHOD,
		&method_ok,
		&s
	);

	res = (amqp_rpc_reply_t) result;	 

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char **pstr = (char **)&str;
		amqp_error(res, pstr); 
		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int queue::purge(name);
purge queue 
*/
PHP_METHOD(amqp_queue_class, purge)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len=0;

	amqp_rpc_reply_t res;
	amqp_rpc_reply_t result;
	amqp_queue_purge_ok_t *r;
	amqp_queue_purge_t s;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|s", &id,
	amqp_queue_class_entry, &name, &name_len) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry,  "Could not purge queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	if (name_len) {
		s.ticket = 0;
		s.queue.len = name_len;
		s.queue.bytes = name;
		s.nowait = 0;
	} else {
		s.ticket = 0;
		s.queue.len = ctx->name_len;
		s.queue.bytes = ctx->name;
		s.nowait = 0;
	}

	amqp_method_number_t method_ok = AMQP_QUEUE_PURGE_OK_METHOD;
	result = amqp_simple_rpc(cnn->conn, 
		AMQP_CHANNEL, 
		AMQP_QUEUE_PURGE_METHOD,
		&method_ok, 
		&s
	);

	res = (amqp_rpc_reply_t) result;	 

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char **pstr = (char **)&str;
		amqp_error(res, pstr); 
		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */



/* {{{ proto int queue::bind(string exchangeName, string routingKey);
bind queue to exchange by routing key  
*/
PHP_METHOD(amqp_queue_class, bind)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len;
	char *exchange_name;
	int exchange_name_len;
	char *keyname;
	int keyname_len;

	amqp_rpc_reply_t res;
	amqp_rpc_reply_t result;	

	amqp_basic_properties_t props;
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss", &id,
	amqp_queue_class_entry, &exchange_name, &exchange_name_len, &keyname, &keyname_len) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not bind queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	amqp_queue_bind_t s;
	s.ticket = 0;
	s.queue.len			 = ctx->name_len;
	s.queue.bytes		   = ctx->name;
	s.exchange.len		  = exchange_name_len;
	s.exchange.bytes		= exchange_name;
	s.routing_key.len	   = keyname_len;
	s.routing_key.bytes	 = keyname;
	s.nowait				= 0;
	s.arguments.num_entries = 0;
	s.arguments.entries	 = NULL;

	amqp_method_number_t bind_ok = AMQP_QUEUE_BIND_OK_METHOD;

	res = (amqp_rpc_reply_t) amqp_simple_rpc(cnn->conn,
		AMQP_CHANNEL,
		AMQP_QUEUE_BIND_METHOD,
		&bind_ok,
		&s
	);

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char **pstr = (char **)&str;
		amqp_error(res, pstr); 

		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int queue::ubind(string exchangeName, string routingKey);
unbind queue from exchange
*/
PHP_METHOD(amqp_queue_class, unbind)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len;
	char *exchange_name;
	int exchange_name_len;
	char *keyname;
	int keyname_len;

	amqp_rpc_reply_t res;

	amqp_basic_properties_t props;
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss", &id,
	amqp_queue_class_entry, &exchange_name, &exchange_name_len, &keyname, &keyname_len) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not unbind queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	amqp_queue_unbind_t s;
	s.ticket				= 0,
	s.queue.len			 = ctx->name_len;
	s.queue.bytes		   = ctx->name;
	s.exchange.len		  = exchange_name_len;
	s.exchange.bytes		= exchange_name;
	s.routing_key.len	   = keyname_len;
	s.routing_key.bytes	 = keyname;
	s.arguments.num_entries = 0;
	s.arguments.entries	 = NULL;

	amqp_method_number_t method_ok = AMQP_QUEUE_UNBIND_OK_METHOD;

	res = (amqp_rpc_reply_t) amqp_simple_rpc(cnn->conn,
		AMQP_CHANNEL,
		AMQP_QUEUE_UNBIND_METHOD,
		&method_ok,
		&s);

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 

		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */



/* {{{ proto array queue::consume(int queue_len, [ bit params=AMQP_NOASK ]);
consume the message
return array messages
*/
PHP_METHOD(amqp_queue_class, consume)
{
	zval *id;
	amqp_queue_object *ctx;
	char *name;
	int name_len;
	int queue_len;
	amqp_rpc_reply_t res;

	char *pbuf;
	long parms = 0;

	zval content;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Ol|l", &id,
	amqp_queue_class_entry, &queue_len, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not consume from queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	if (!parms) {
		parms = AMQP_NOACK;
	}

	amqp_basic_consume_t s;
	s.ticket				= 0;
	s.queue.len			 = ctx->name_len;
	s.queue.bytes		   = ctx->name;
	s.consumer_tag.len	  = 0;
	s.consumer_tag.bytes	= NULL;
	s.no_local			  = (AMQP_NOLOCAL & parms) ? 1 : 0;
	s.no_ack				= (AMQP_NOACK & parms) ? 1 : 0;
	s.exclusive			 = (AMQP_EXCLUSIVE & parms) ? 1 : 0;
	s.nowait				= 0;

	amqp_method_number_t method_ok = AMQP_BASIC_CONSUME_OK_METHOD;  
	res = (amqp_rpc_reply_t)amqp_simple_rpc(cnn->conn, 
		AMQP_CHANNEL, 
		AMQP_BASIC_CONSUME_METHOD, 
		&method_ok, 
		&s);

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		cnn->is_channel_connected=0;
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr);
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	amqp_basic_consume_ok_t *r = (amqp_basic_consume_ok_t *) res.reply.decoded;

	memcpy(ctx->consumer_tag, r->consumer_tag.bytes, r->consumer_tag.len);
	ctx->consumer_tag_len = r->consumer_tag.len;

	int received = 0;
	int previous_received = 0;

	amqp_frame_t frame;
	int result;
	size_t body_received;
	size_t body_target;
	int i;
	array_init(return_value);
	char *buf = NULL;

	for (i = 0; i < queue_len; i++) {

		amqp_maybe_release_buffers(cnn->conn);
		result = amqp_simple_wait_frame(cnn->conn, &frame);
		if (result <= 0) {
			return;
		}

		if (frame.frame_type != AMQP_FRAME_METHOD) {
			continue;
		}

		if (frame.payload.method.id != AMQP_BASIC_DELIVER_METHOD) {
			continue;
		}

		result = amqp_simple_wait_frame(cnn->conn, &frame);
		if (result <= 0) {
			zend_throw_exception(amqp_queue_exception_class_entry, "The returned read frame is invalid.", 0 TSRMLS_CC);
			return;
		}

		if (frame.frame_type != AMQP_FRAME_HEADER) {
			zend_throw_exception(amqp_queue_exception_class_entry, "The returned frame type is invalid.", 0 TSRMLS_CC);
			return;
		}

		body_target = frame.payload.properties.body_size;
		body_received = 0;

		buf = (char *)emalloc(frame.payload.properties.body_size);
		pbuf = buf;
		while (body_received < body_target) {
			result = amqp_simple_wait_frame(cnn->conn, &frame);
			if (result <= 0) {
				break;
			}

			if (frame.frame_type != AMQP_FRAME_BODY) {
				zend_throw_exception(amqp_queue_exception_class_entry, "The returned frame has no body.", 0 TSRMLS_CC);
				return;
			}

			body_received += frame.payload.body_fragment.len;

			memcpy(pbuf, frame.payload.body_fragment.bytes, frame.payload.body_fragment.len);
			pbuf += frame.payload.body_fragment.len;

		} /* end while  */

		add_index_stringl(return_value, i, buf, body_received, 1);	
		efree(buf);
	}

}
/* }}} */



/* {{{ proto int queue::get([ bit params=AMQP_NOASK ]);
read message from queue
return array (count_in_queue, message) 
*/
PHP_METHOD(amqp_queue_class, get)
{
	zval *id;
	amqp_queue_object *ctx;
	char *type=NULL;
	int	type_len;
	amqp_rpc_reply_t res;

	char str[256];
	char **pstr = (char **)&str;
	long parms = AMQP_NOACK;

	zval content;

	amqp_basic_get_ok_t *get_ok;
	amqp_channel_close_t *err;

	int result;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|l", &id, amqp_queue_class_entry, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not get from queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	amqp_basic_get_t s;
	s.ticket = 0,
	s.queue.len = ctx->name_len;
	s.queue.bytes = ctx->name;
	s.no_ack = (AMQP_NOACK & parms) ? 1 : 0;

	int status = amqp_send_method(cnn->conn, 
		AMQP_CHANNEL, 
		AMQP_BASIC_GET_METHOD,  
		&s
	);

	int count=0;
	amqp_frame_t frame;

	array_init(return_value);

	size_t len=0;
	char *tmp = NULL;
	char *old_tmp = NULL;

	while (1) { /* receive  frames:  */

		amqp_maybe_release_buffers(cnn->conn);
		result = amqp_simple_wait_frame(cnn->conn, &frame);

		if (result <= 0) {
			RETURN_FALSE;
		}

		if (frame.frame_type == AMQP_FRAME_METHOD) {

			if (AMQP_BASIC_GET_OK_METHOD == frame.payload.method.id) {

				get_ok = (amqp_basic_get_ok_t *) frame.payload.method.decoded;
				count = get_ok->message_count;

				add_assoc_stringl_ex(
					return_value,
					"routing_key",
					12,
					get_ok->routing_key.bytes,
					get_ok->routing_key.len,
					1
				);

				add_assoc_stringl_ex(
					return_value,
					"exchange",
					9,
					get_ok->exchange.bytes,
					get_ok->exchange.len,
					1
				);
				
				add_assoc_long_ex(
					return_value,
					"delivery_tag",
					13,
					get_ok->delivery_tag
				);
			}

			if (AMQP_CHANNEL_CLOSE_OK_METHOD == frame.payload.method.id) {
				err = (amqp_channel_close_t *)frame.payload.method.decoded;
				spprintf(pstr, 0, "Server error: %d", (int)err->reply_code);
				zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
				return;
			}

			if (AMQP_BASIC_GET_EMPTY_METHOD == frame.payload.method.id ) {
				count = -1;
				break;
			}

			continue;

		} //* ------ end GET_OK */

		if (frame.frame_type == AMQP_FRAME_HEADER) {

			amqp_basic_properties_t *p = (amqp_basic_properties_t *) frame.payload.properties.decoded;

			if (p->_flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
				add_assoc_stringl_ex(return_value,
					"Content-type",
					13,
					p->content_type.bytes,
					p->content_type.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_CONTENT_ENCODING_FLAG) {
				add_assoc_stringl_ex(return_value,
					"Content-encoding",
					sizeof("Content-encoding"),
					p->content_encoding.bytes,
					p->content_encoding.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_TYPE_FLAG) {
				add_assoc_stringl_ex(return_value,
					"type",
					sizeof("type"),
					p->type.bytes,
					p->type.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_TIMESTAMP_FLAG) {
				add_assoc_long(return_value, "timestamp", p->timestamp);
			}

			if (p->_flags & AMQP_BASIC_PRIORITY_FLAG) {
				add_assoc_long(return_value, "priority", p->priority);
			}

			if (p->_flags & AMQP_BASIC_EXPIRATION_FLAG) {
				add_assoc_stringl_ex(return_value,
					"expiration",
					sizeof("expiration"),
					p->expiration.bytes,
					p->expiration.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_USER_ID_FLAG) {
				add_assoc_stringl_ex(return_value,
					"user_id",
					sizeof("user_id"),
					p->user_id.bytes,
					p->user_id.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_APP_ID_FLAG) {
				add_assoc_stringl_ex(return_value,
					"app_id",
					sizeof("app_id"),
					p->app_id.bytes,
					p->app_id.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_MESSAGE_ID_FLAG) {
				add_assoc_stringl_ex(return_value,
					"mesage_id",
					sizeof("message_id"),
					p->message_id.bytes,
					p->message_id.len,
					1
				);
			}

			if (p->_flags & AMQP_BASIC_REPLY_TO_FLAG) {
				add_assoc_stringl_ex(return_value,
					"Reply-to",
					sizeof("Reply-to"),
					p->reply_to.bytes,
					p->reply_to.len,
					1
				);
			}

			if (frame.payload.properties.body_size==0) {				
				break;
			}
			continue;
		}	  

		if (frame.frame_type == AMQP_FRAME_BODY) {

			uint frame_len = frame.payload.body_fragment.len;	
			size_t old_len = len;
			len += frame_len;

			if (tmp) {
				old_tmp = tmp;
				tmp = (char *)emalloc(len);
				memcpy(tmp, old_tmp, old_len);
				efree(old_tmp);
				memcpy(tmp + old_len,frame.payload.body_fragment.bytes, frame_len);
			} else { /* the first allocate */
				tmp = (char *)estrdup(frame.payload.body_fragment.bytes);  
			}

			if (frame_len < FRAME_MAX - HEADER_FOOTER_SIZE) {
				break;
			}

			continue;
		}	  

	} /* end while */

	add_assoc_long(return_value, "count",count);

	if (count > -1) {		  
		add_assoc_stringl_ex(return_value,
			"msg",
			4,
			tmp,
			len,
			1
		);
		efree(tmp);
	}
}
/* }}} */



/* {{{ proto int queue::cancel(consumer_tag);
cancel queue to consumer
*/
PHP_METHOD(amqp_queue_class, cancel)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	char *consumer_tag = NULL;
	int consumer_tag_len=0;
	amqp_rpc_reply_t res;
	amqp_rpc_reply_t result;	
	amqp_basic_cancel_t s;

	amqp_basic_properties_t props;
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|s", &id,
	amqp_queue_class_entry, &consumer_tag, &consumer_tag_len) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not cancel queue. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *)zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	if (consumer_tag_len) {
		s.consumer_tag.len = consumer_tag_len;
		s.consumer_tag.bytes = consumer_tag;
		s.nowait = 0;
	} else {
		s.consumer_tag.len = ctx->consumer_tag_len;
		s.consumer_tag.bytes = ctx->consumer_tag;
		s.nowait = 0;
	}

	amqp_method_number_t method_ok = AMQP_BASIC_CANCEL_OK_METHOD;

	result = amqp_simple_rpc(cnn->conn,
		AMQP_CHANNEL,
		AMQP_BASIC_CANCEL_METHOD,
		&method_ok,
		&s
	);

	res = (amqp_rpc_reply_t)result;	

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char **pstr = (char **)&str;
		amqp_error(res, pstr); 
		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_queue_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int queue::ack(long deliveryTag, [bit params=AMQP_NONE]);
	acknowledge the message
*/
PHP_METHOD(amqp_queue_class, ack)
{
	zval *id;
	amqp_queue_object *ctx;
	amqp_object *ctx_cnn;
	long deliveryTag = 0;
	long parms = 0;
	
	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Ol|l", &id, amqp_queue_class_entry, &deliveryTag, &parms ) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_queue_object *)zend_object_store_get_object(id TSRMLS_CC);
	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_queue_exception_class_entry, "Could not ack message. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object * cnn = (amqp_object *)zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	amqp_basic_ack_t s;
	s.delivery_tag = deliveryTag;
	s.multiple = ( AMQP_MULTIPLE & parms ) ? 1 : 0;

	int res = amqp_send_method(cnn->conn,
				AMQP_CHANNEL,
				AMQP_BASIC_ACK_METHOD,
				&s);

	if (res) {
		zend_throw_exception_ex(amqp_queue_exception_class_entry, 0 TSRMLS_CC, "Ack error; code=%d", res);
		return;
	}

	RETURN_TRUE;
}
/* }}} */



/* ------------------  class Exchange ----------- */

/* {{{ proto AMQPEexchange( AMQPConnection cnn, [string name]);  //////, string type=direct, [ bit params ]);
declare Exchange   */
PHP_METHOD(amqp_exchange_class, __construct)
{
	zval *id;
	zval *cnnOb;
	amqp_exchange_object *ctx;
	amqp_object *ctx_cnn;

	char *name;
	int name_len = 0;
	amqp_rpc_reply_t res;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oo|s", &id, amqp_exchange_class_entry, &cnnOb, &name, &name_len) == FAILURE) {
		RETURN_FALSE;
	}

	php_std_error_handling();
	if (!instanceof_function(Z_OBJCE_P(cnnOb), amqp_class_entry TSRMLS_CC)) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "The first parameter must be and instance of AMQPConnection.", 0 TSRMLS_CC);
		return;
	} 

	ctx = (amqp_exchange_object *)zend_object_store_get_object(id TSRMLS_CC);
	ctx->cnn = cnnOb;

	if(!cnnOb) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "The given AMQPConnection object is null.", 0 TSRMLS_CC);
		return;
	}

	if (name_len) {
		AMQP_SET_NAME(ctx, name);
	}

	/* We have a valid connection: */
	ctx->is_connected = '\1';

	ctx_cnn = (amqp_object *)zend_object_store_get_object(cnnOb TSRMLS_CC);		  

	RETURN_FALSE;
}
/* }}} */


/* {{{ proto AMQPExchange::declare( [string name], [string type=direct], [ bit params ]);
declare Exchange   
*/
PHP_METHOD(amqp_exchange_class, declare)
{
	zval *id;
	zval *cnnOb;

	amqp_exchange_object *ctx;
	amqp_object *ctx_cnn;

	char *name;
	int name_len = 0;
	char *type;
	int type_len = 0;
	long parms = 0;

	amqp_rpc_reply_t res;
	amqp_exchange_declare_t s;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|ssl", &id, amqp_exchange_class_entry, &name, &name_len, &type, &type_len, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_exchange_object *)zend_object_store_get_object(id TSRMLS_CC);

	if (!type_len) {
		type = AMQP_EX_TYPE_DIRECT;	/* default - direct */
		type_len = 6;	//strlen('direct')
	}	

	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_exchange_exception_class_entry, "Could not declare exchange. No connection available.", 0 TSRMLS_CC);
		return;
	}

	ctx_cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC); 

	if(!ctx_cnn) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "The given AMQPConnection object is null.", 0 TSRMLS_CC);
		return;
	}

	amqp_bytes_t amqp_name;
	if (name_len) {
		AMQP_SET_NAME(ctx, name);
		amqp_name.len = name_len;
		amqp_name.bytes = name;
	} else {
		amqp_name.len = ctx->name_len;
		amqp_name.bytes = ctx->name;
	}

	amqp_bytes_t amqp_type;
	amqp_type.len = type_len;
	amqp_type.bytes = type;

	AMQP_NULLARGS
	AMQP_PASSIVE_D
	AMQP_DURABLE_D
	AMQP_AUTODELETE_D

	res = AMQP_SIMPLE_RPC(ctx_cnn->conn, 
		AMQP_CHANNEL,  EXCHANGE, DECLARE, DECLARE_OK,
		amqp_exchange_declare_t,
		0, amqp_name, amqp_type, passive, durable, auto_delete, 0, 0, arguments
	);


	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 
		zend_throw_exception(amqp_exchange_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;

}
/* }}} */



/* {{{ proto AMQPEexchange::delete([string name]);
delete Exchange   
*/
PHP_METHOD(amqp_exchange_class, delete)
{
	zval *id;
	zval *cnnOb;

	amqp_exchange_object *ctx;
	amqp_object *ctx_cnn;

	char *name;
	int name_len = 0;
	long parms = 0;

	amqp_rpc_reply_t res;
	amqp_exchange_delete_t s;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O|sl", &id, amqp_exchange_class_entry, &name, &name_len, &parms) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_exchange_object *)zend_object_store_get_object(id TSRMLS_CC);

	if (name_len) {
		AMQP_SET_NAME(ctx, name);

		s.ticket = 0;
		s.exchange.len = name_len;
		s.exchange.bytes = name;
		s.if_unused = (AMQP_IFUNUSED & parms) ? 1 : 0;
		s.nowait = 0;
	} else {
		s.ticket = 0,
		s.exchange.len = ctx->name_len;
		s.exchange.bytes = ctx->name;
		s.if_unused = (AMQP_IFUNUSED & parms) ? 1 : 0;
		s.nowait = 0;
	}

/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_exchange_exception_class_entry, "Could not delete exchange. No connection available.", 0 TSRMLS_CC);
		return;
	}

	ctx_cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC); 
	if(!ctx_cnn) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "The given AMQPConnection object is null.", 0 TSRMLS_CC);
		return;
	}

	amqp_method_number_t method_ok = AMQP_EXCHANGE_DELETE_OK_METHOD; 

	res = amqp_simple_rpc(ctx_cnn->conn, AMQP_CHANNEL, 
		AMQP_EXCHANGE_DELETE_METHOD, 
		&method_ok, &s
	);

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 
		zend_throw_exception(amqp_exchange_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto AMQPEexchange::publish(string msg, [string key]);
publish into Exchange   
*/
PHP_METHOD(amqp_exchange_class, publish)
{
	zval *id;
	zval *cnnOb;
	zval *iniArr = NULL;
	zval** zdata;
	amqp_exchange_object *ctx;
	amqp_object *ctx_cnn;

	char *key_name = NULL;
	int key_len = 0;

	char *msg;
	int msg_len= 0;

	long parms = 0;

	amqp_rpc_reply_t res;

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os|sla", &id, amqp_exchange_class_entry, &msg, &msg_len, &key_name, &key_len, &parms, &iniArr) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_exchange_object *)zend_object_store_get_object(id TSRMLS_CC);

	if (!ctx->name_len) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "Please provide an exchange name.", 0 TSRMLS_CC);
		return;
	}

	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_exchange_exception_class_entry, "Could not publish to exchange. No connection available.", 0 TSRMLS_CC);
		return;
	}

	ctx_cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC); 

	if(!ctx_cnn) {
		zend_throw_exception(amqp_exchange_exception_class_entry, "The given AMQPConnection object is null.", 0 TSRMLS_CC);
		return;
	}

	amqp_basic_properties_t props;

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "Content-type", sizeof("Content-type"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.content_type = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));		  
	} else {
		props.content_type = amqp_cstring_bytes("text/plain");
	}

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "Content-encoding", sizeof("Content-encoding"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.content_encoding = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_CONTENT_ENCODING_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "message_id", sizeof("message_id"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.message_id = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_MESSAGE_ID_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "user_id", sizeof("user_id"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.user_id = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_USER_ID_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "app_id", sizeof("app_id"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.app_id = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_APP_ID_FLAG;
	}		 


	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "delivery_mode", sizeof("delivery_mode"), (void*)&zdata)) {
		props.delivery_mode = (uint8_t)Z_LVAL_PP(zdata);
		props._flags += AMQP_BASIC_DELIVERY_MODE_FLAG;
	}		 


	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "priority", sizeof("priority"), (void*)&zdata)) {
		props.priority = (uint8_t)Z_LVAL_PP(zdata);
		props._flags += AMQP_BASIC_PRIORITY_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "timestamp", sizeof("timestamp"), (void*)&zdata)) {
		props.timestamp = (uint64_t)Z_LVAL_PP(zdata);
		props._flags += AMQP_BASIC_TIMESTAMP_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "expiration", sizeof("expiration"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.expiration =  amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_EXPIRATION_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "type", sizeof("type"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.type =  amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_TYPE_FLAG;
	}		 

	if (iniArr && SUCCESS == zend_hash_find(HASH_OF (iniArr), "reply_to", sizeof("reply_to"), (void*)&zdata)  && strlen(Z_STRVAL_PP(zdata)) > 0 ) {
		props.reply_to = amqp_cstring_bytes((char*)Z_STRVAL_PP(zdata));
		props._flags += AMQP_BASIC_REPLY_TO_FLAG;
	}		 


	int r = amqp_basic_publish(ctx_cnn->conn,
		AMQP_CHANNEL,
		(amqp_bytes_t) {ctx->name_len, ctx->name},
		(amqp_bytes_t) {key_len, key_name },
		(AMQP_MANDATORY & parms) ? 1 : 0, /* mandatory */
		(AMQP_IMMEDIATE & parms) ? 1 : 0, /* immediate */
		&props,
		(amqp_bytes_t) {msg_len, msg });

	if (r) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 
		zend_throw_exception(amqp_exchange_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int exchange::bind(string queueName, string routingKey);
bind exchange to queue by routing key  
*/
PHP_METHOD(amqp_exchange_class, bind)
{
	zval *id;
	amqp_exchange_object *ctx;
	amqp_object *ctx_cnn;
	char *name;
	int name_len;
	char *queue_name;
	int queue_name_len;
	char *keyname;
	int keyname_len;

	amqp_rpc_reply_t res;
	amqp_rpc_reply_t result;	

	if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss", &id, amqp_exchange_class_entry, &queue_name, &queue_name_len, &keyname, &keyname_len) == FAILURE) {
		RETURN_FALSE;
	}

	ctx = (amqp_exchange_object *)zend_object_store_get_object(id TSRMLS_CC);

	/* Check that the given connection has a channel, before trying to pull the connection off the stack */
	if (ctx->is_connected != '\1') {
		zend_throw_exception(amqp_exchange_exception_class_entry, "Could not bind exchange. No connection available.", 0 TSRMLS_CC);
		return;
	}

	amqp_object *cnn = (amqp_object *) zend_object_store_get_object(ctx->cnn TSRMLS_CC);

	amqp_queue_bind_t s;
	s.ticket				= 0;
	s.queue.len			 = queue_name_len;
	s.queue.bytes		   = queue_name;
	s.exchange.len		  = ctx->name_len;
	s.exchange.bytes		= ctx->name;
	s.routing_key.len	   = keyname_len;
	s.routing_key.bytes	 = keyname;
	s.nowait				= 0;
	s.arguments.num_entries = 0;
	s.arguments.entries	 = NULL;

	amqp_method_number_t method_ok = AMQP_QUEUE_BIND_OK_METHOD;		
	result = amqp_simple_rpc(cnn->conn,
		AMQP_CHANNEL,
		AMQP_QUEUE_BIND_METHOD,
		&method_ok,
		&s);

	res = (amqp_rpc_reply_t)result;	

	if (res.reply_type != AMQP_RESPONSE_NORMAL) {
		char str[256];
		char ** pstr = (char **) &str;
		amqp_error(res, pstr); 
		cnn->is_channel_connected = 0;
		zend_throw_exception(amqp_exchange_exception_class_entry, *pstr, 0 TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */


///---------------------


static void amqp_dtor(void *object TSRMLS_DC)
{
	/*
	If we are trying to close the connection and the connection already closed, it will throw
	SIGPIPE, which is fine, so ignore all SIGPIPES
	*/

	/* Start ignoring SIGPIPE */
	int handler = signal(SIGPIPE, SIG_IGN);
	
	amqp_object *ob = (amqp_object*)object;

	if (ob->is_channel_connected) {
		amqp_channel_close(ob->conn, AMQP_CHANNEL, AMQP_REPLY_SUCCESS);
	}

	if (ob->conn && ob->is_connected) {
		amqp_destroy_connection(ob->conn);
	}

	if (ob->fd) {
		close(ob->fd);
	}

	/* End ignoring of SIGPIPEs */
	signal(SIGPIPE, handler);

	zend_object_std_dtor(&ob->zo TSRMLS_CC);
	
	efree(object);
}

static zend_object_value amqp_ctor(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value new_value;
	amqp_object* obj = (amqp_object*)emalloc(sizeof(amqp_object));

	memset(obj, 0, sizeof(amqp_object));

	zend_object_std_init(&obj->zo, ce TSRMLS_CC);

	new_value.handle = zend_objects_store_put(obj, (zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)amqp_dtor, NULL TSRMLS_CC);
	new_value.handlers = zend_get_std_object_handlers();

	return new_value;
}


static void amqp_queue_dtor(void *object TSRMLS_DC)
{
	amqp_queue_object *ob = (amqp_queue_object*)object;

	efree(object);
}

static zend_object_value amqp_queue_ctor(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value new_value;
	amqp_queue_object* obj = (amqp_queue_object*)emalloc(sizeof(amqp_queue_object));

	memset(obj, 0, sizeof(amqp_queue_object));

	zend_object_std_init(&obj->zo, ce TSRMLS_CC);

	new_value.handle = zend_objects_store_put(obj, (zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)amqp_queue_dtor, NULL TSRMLS_CC);
	new_value.handlers = zend_get_std_object_handlers();

	return new_value;
}


static void amqp_exchange_dtor(void *object TSRMLS_DC)
{		  
	amqp_exchange_object *ob = (amqp_exchange_object*)object;

	efree(object);
}

static zend_object_value amqp_exchange_ctor(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value new_value;
	amqp_queue_object* obj = (amqp_queue_object*)emalloc(sizeof(amqp_queue_object));

	memset(obj, 0, sizeof(amqp_exchange_object));

	zend_object_std_init(&obj->zo, ce TSRMLS_CC);

	new_value.handle = zend_objects_store_put(obj, (zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)amqp_exchange_dtor, NULL TSRMLS_CC);
	new_value.handlers = zend_get_std_object_handlers();

	return new_value;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("amqp.host",	  "localhost",	PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("amqp.vhost",	 "/",			PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("amqp.port",	  PORT_STR,	   PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("amqp.login",	 "guest",		PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("amqp.password",  "guest",		PHP_INI_ALL, NULL)
	PHP_INI_END()

/* {{{ PHP_MINIT_FUNCTION
*/
PHP_MINIT_FUNCTION(amqp)
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "AMQPConnection", amqp_class_functions);
	ce.create_object = amqp_ctor;
	amqp_class_entry = zend_register_internal_class(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPQueue", amqp_queue_class_functions);
	ce.create_object = amqp_queue_ctor;
	amqp_queue_class_entry = zend_register_internal_class(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPExchange", amqp_exchange_class_functions);
	ce.create_object = amqp_exchange_ctor;
	amqp_exchange_class_entry = zend_register_internal_class(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPException", NULL);
	amqp_exception_class_entry = zend_register_internal_class_ex(&ce, (zend_class_entry*)zend_exception_get_default(TSRMLS_C), NULL TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPConnectionException", NULL);
	amqp_connection_exception_class_entry = zend_register_internal_class_ex(&ce, amqp_exception_class_entry, NULL TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPExchangeException", NULL);
	amqp_exchange_exception_class_entry = zend_register_internal_class_ex(&ce, amqp_exception_class_entry, NULL TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "AMQPQueueException", NULL);
	amqp_queue_exception_class_entry = zend_register_internal_class_ex(&ce, amqp_exception_class_entry, NULL TSRMLS_CC);

	REGISTER_INI_ENTRIES();

	REGISTER_LONG_CONSTANT("AMQP_DURABLE",	  AMQP_DURABLE,	   CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_PASSIVE",	  AMQP_PASSIVE,	   CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_EXCLUSIVE",	AMQP_EXCLUSIVE,	 CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_AUTODELETE",   AMQP_AUTODELETE,	CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_INTERNAL",	 AMQP_INTERNAL,	  CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_NOLOCAL",	  AMQP_NOLOCAL,	   CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_NOACK",		AMQP_NOACK,		 CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_IFEMPTY",	  AMQP_IFEMPTY,	   CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_IFUNUSED",	 AMQP_IFUNUSED,	  CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_MANDATORY",	AMQP_MANDATORY,	 CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_IMMEDIATE",	AMQP_IMMEDIATE,	 CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("AMQP_MULTIPLE",	 AMQP_MULTIPLE,  CONST_CS | CONST_PERSISTENT);

	REGISTER_STRING_CONSTANT("AMQP_EX_TYPE_DIRECT", AMQP_EX_TYPE_DIRECT,	CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("AMQP_EX_TYPE_FANOUT", AMQP_EX_TYPE_FANOUT,	CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("AMQP_EX_TYPE_TOPIC",  AMQP_EX_TYPE_TOPIC,	 CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("AMQP_EX_TYPE_HEADER", AMQP_EX_TYPE_HEADER,	CONST_CS | CONST_PERSISTENT);

	return SUCCESS;

}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
*/
PHP_MSHUTDOWN_FUNCTION(amqp)
{
	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}
/* }}} */


/* {{{ PHP_MINFO_FUNCTION
*/
PHP_MINFO_FUNCTION(amqp)
{
	/* Build date time from compiler macros */
	char datetime[32];
	char **pstr = (char **)&datetime;
	spprintf(pstr, 0, "%s @ %s", __DATE__, __TIME__);

	php_info_print_table_start();
	php_info_print_table_header(2, "Version",			   "$Revision$");
	php_info_print_table_header(2, "Compiled",			  *pstr);
	php_info_print_table_header(2, "AMQP protocol version", "8.0");
	php_info_print_table_header(2, "Default host",		  "localhost");
	php_info_print_table_header(2, "Default virtual host",  "/");
	php_info_print_table_header(2, "Default port",		  PORT_STR);	
	php_info_print_table_header(2, "Default login",		 "guest");	
	php_info_print_table_header(2, "Default password",	  "guest");	
	php_info_print_table_end();

}
/* }}} */



/*
*Local variables:
*tab-width: 4
*c-basic-offset: 4
*End:
*vim600: noet sw=4 ts=4 fdm=marker
*vim<600: noet sw=4 ts=4
*/
