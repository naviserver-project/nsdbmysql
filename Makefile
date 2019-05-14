
ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Install MySQL client library on Debian/Ubuntu:
#      apt install libmysqlclient-dev
#
# This installs essentially the following files
#      /usr/include/mysql/mysql.h
#      /usr/lib/x86_64-linux-gnu/libmysqlclient.so
#
# Typically, MySQL installs also a script called "mysql_config", which
# can be used to determine the include and library paths
# automatically. If this is not available on your system you have to
# set the paths manually like in the example below for macOS.
#
MYSQLINCLUDE = $(shell mysql_config --include)
MYSQLLIBRARY = $(shell mysql_config --libs)

#
# On macOS: port install mysql57
#
# MYSQLINCLUDE = -I/opt/local/include/mysql57/mysql/
# MYSQLLIBRARY = -L/opt/local/lib/mysql57/mysql

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
MODLIBS  =  $(MYSQLLIBRARY) -lnsdb -lmysqlclient

#
# Compiler flags
#
CFLAGS   = $(MYSQLINCLUDE)


include  $(NAVISERVER)/include/Makefile.module
