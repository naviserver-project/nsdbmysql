
ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD       =  nsdbmysql.so

#
# Objects to build.
#
MODOBJS      =  nsdbmysql.o

#
# Header files in THIS directory
#
HDRS     =

#
# Extra libraries
#
MODLIBS  =  -lmysqlclient_r

#
# Compiler flags
#
CFLAGS   = -I/usr/include/mysql
#CFLAGS   = -I/opt/local/include/mysql57/mysql/

include  $(NAVISERVER)/include/Makefile.module
