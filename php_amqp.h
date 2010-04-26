/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2007 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Alexandre Kalendarev akalend@mail.ru Copyright (c) 2009-2010 |
  | Contributor: Pieter de Zwart pdezwart@php.net                        |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifndef PHP_AMQP_H
#define PHP_AMQP_H

extern zend_module_entry amqp_module_entry;
#define phpext_amqp_ptr &amqp_module_entry

#ifdef PHP_WIN32
#define PHP_AMQP_API __declspec(dllexport)
#else
#define PHP_AMQP_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

#define AMQP_NOPARM		1

#define AMQP_DURABLE		2
#define AMQP_PASSIVE		4
#define AMQP_EXCLUSIVE		8
#define AMQP_AUTODELETE	16
#define AMQP_INTERNAL		32
#define AMQP_NOLOCAL		64
#define AMQP_NOACK			128
#define AMQP_IFEMPTY		256
#define AMQP_IFUNUSED		528
#define AMQP_MANDATORY		1024
#define AMQP_IMMEDIATE		2048
#define AMQP_MULTIPLE       4096

#define AMQP_EX_TYPE_DIRECT     "direct"
#define AMQP_EX_TYPE_FANOUT     "fanout"
#define AMQP_EX_TYPE_TOPIC      "topic"
#define AMQP_EX_TYPE_HEADER     "header"


PHP_MINIT_FUNCTION(amqp);
PHP_MSHUTDOWN_FUNCTION(amqp);
PHP_MINFO_FUNCTION(amqp);
	
PHP_METHOD(amqp_class, __construct);
PHP_METHOD(amqp_class, isConnected);

PHP_METHOD(amqp_queue_class, __construct);
PHP_METHOD(amqp_queue_class, declare);
PHP_METHOD(amqp_queue_class, consume);
PHP_METHOD(amqp_queue_class, delete);
PHP_METHOD(amqp_queue_class, purge);
PHP_METHOD(amqp_queue_class, bind);
PHP_METHOD(amqp_queue_class, unbind);
PHP_METHOD(amqp_queue_class, get);
PHP_METHOD(amqp_queue_class, cancel);
PHP_METHOD(amqp_queue_class, ack);

PHP_METHOD(amqp_exchange_class, __construct);
PHP_METHOD(amqp_exchange_class, declare);
PHP_METHOD(amqp_exchange_class, delete);
PHP_METHOD(amqp_exchange_class, bind);
PHP_METHOD(amqp_exchange_class, publish);

#ifdef ZTS
#define AMQP_G(v) TSRMG(amqp_globals_id, zend_amqp_globals *, v)
#else
#define AMQP_G(v) (amqp_globals.v)
#endif

#endif	/* PHP_AMQP_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
