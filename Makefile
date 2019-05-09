
ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# For Debian/Ubuntu:
#      apt install libmysqlclient-dev
# installs into
#      /usr/include/mysql/mysql.h
#      /usr/lib/x86_64-linux-gnu/libmysqlclient.so
#
MYSQLINCLUDE = -I/usr/include/mysql
MYSQLLIBRARY =

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
