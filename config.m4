dnl $Id$
dnl config.m4 for extension rabbit

dnl Comments in this file start with the string 'dnl'.
dnl Remove where necessary. This file will not work
dnl without editing.
amqp
dnl If your extension references something external, use with:


dnl Make sure that the comment is aligned:
 PHP_ARG_WITH(amqp, for amqp support,
 [  --with-amqp             Include amqp support])


if test "$PHP_AMQP" != "no"; then
  dnl Write more examples of tests here...

  dnl # --with-amqp -> check with-path
  
         SEARCH_PATH="/usr/local"     # you might want to change this

  	 #SEARCH_FOR="/usr/local/include/amqp.h"  
  	 #SEARCH_FOR="/usr/local/include/amqp_framing.h"  
         SEARCH_FOR="amqp_framing.h"  

    dnl AC_MSG_CHECKING($SEARCH_FOR)
     AC_MSG_CHECKING([for amqp files in default path])
     AC_MSG_RESULT($INCLUDE_PATH)


      for i in $SEARCH_PATH ; do
       if test -r $i/include/$SEARCH_FOR; 
             AC_MSG_RESULT(test $i/include/$SEARCH_FOR) 
       then
            AMQP_DIR=$i
            AC_MSG_RESULT(found in $i)
         fi
       done

     AC_MSG_CHECKING([path $AMQP_DIR ])
     AC_MSG_RESULT($AMQP_DIR)


    if test -z "$AMQP_DIR"; then
       AC_MSG_RESULT([not found])
       AC_MSG_ERROR([Please reinstall the amqp distribution])
    fi

  dnl # --with-amqp -> add include path
  
  PHP_ADD_INCLUDE($AMQP_DIR/include)

  dnl # --with-amqp -> check for lib and symbol presence

      LIBNAME=rabbitmq # you may want to change this
      LIBSYMBOL=rabbitmq # you most likely want to change this 


   PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $AMQP_DIR/lib, AMQP_SHARED_LIBADD)
   PHP_SUBST(AMQP_SHARED_LIBADD)

 
  PHP_NEW_EXTENSION(amqp, amqp.c, $ext_shared)
fi
