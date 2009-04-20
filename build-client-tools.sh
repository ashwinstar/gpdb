#!/bin/bash

. ${HOME}/bin/lib/platform_defines.sh
if [ $? -ne 0 ] ; then
    echo "Error sourcing ${HOME}//bin/lib/platform_defines.sh"
    exit 1
fi

SCRIPT_EXT=sh
LIB_EXT=so
EXE_EXT=
ANT_OPTS=""

if [ x$1 != x ] && [ $1 = win ] ; then
    WINDOWS=true
    ANT_OPTS="-DminGW=true"
    SCRIPT_EXT=bat
    EXE_EXT=.exe
else
    WINDOWS=false
fi

#Versioning
VER=3.2.0.0

#If gcc is mingw gcc, and "win" is not provided on the command line, exit.
which gcc | grep mingw > /dev/null 2>&1
if [ "$?" = "0" ] && [ "${WINDOWS}" = "false" ] ; then
    echo "Using mingw gcc when compiling unix. Exiting."
    exit 1
fi

WORK_DIR=`pwd`
#Src dirs
ODBC_DIR=${WORK_DIR}/client/odbc
ODBC_SRC=${ODBC_DIR}/src
ODBC_INSTALL=${ODBC_DIR}/install
JDBC_DIR=${WORK_DIR}/client/jdbc

GPDB_DIR=${WORK_DIR}
GPDB_SRC=${GPDB_DIR}
GPDB_INSTALL=${GPDB_DIR}

DOC_DIR=${WORK_DIR}/client/docs
SCRIPT_DIR=${WORK_DIR}/client/scripts

#Install
INSTALL_DIR_NAME=greenplum-clients
INSTALL_DIR=${WORK_DIR}/client/install/${INSTALL_DIR_NAME}
LIB_INSTALL_DIR=${INSTALL_DIR}/lib
JDBC_INSTALL_DIR=${INSTALL_DIR}/drivers/jdbc
ODBC_INSTALL_DIR=${INSTALL_DIR}/drivers/odbc
INCLUDE_DIR=${INSTALL_DIR}/include

#
#Build
#

compileODBC() {
    if [ $WINDOWS = true ] ; then
        echo "Skipping ODBC compile (Not performed for windows)"
    else
        #ODBC needs libpq, which it picks up with pg_config
        PATH=${GPDB_INSTALL}/greenplum-db-devel/bin:${PATH}
        UNIX_ODBC_VER=2.2.11
        UNIX_ODBC=unixODBC-${UNIX_ODBC_VER}
        PSQL_ODBC_VER=08.02.0002
        PSQL_ODBC=psqlodbc-${PSQL_ODBC_VER}

        echo "Starting ${UNIX_ODBC} compile at" `date`
        echo 
        echo "cd ${ODBC_SRC}"
        cd ${ODBC_SRC}
        echo "rm -rf ${UNIX_ODBC}"
        rm -rf ${UNIX_ODBC}
        echo "${TAR} zxf ${UNIX_ODBC}.tar.gz"
        ${TAR} zxf ${UNIX_ODBC}.tar.gz
        echo "Overlaying unixODBC-2.2.11 bug fixes..."
        (cd ${UNIX_ODBC}-fixes && gtar cf - --exclude='*/CVS' --exclude='*/CVS/*' .) | (cd ${UNIX_ODBC} && gtar xvf -)
        echo "...done."
        echo "cd ${UNIX_ODBC}"
        cd ${UNIX_ODBC}
        echo "./configure --enable-gui=no > configure.log 2>&1"
        ./configure --enable-gui=no > configure.log 2>&1
        #fix ODBC's libtool for a bug that shows on OSX
        if [ ${PLAT} = "Darwin" ]; then
            echo "Fixing libtool for safe running on OS X (${PLAT})..."
            sed -e 's,whole_archive_flag_spec="-all_load \\\$convenience",whole_archive_flag_spec=,' libtool > libtool.modified
            cp -p libtool libtool.orig
            cp libtool.modified libtool
            chmod 755 libtool
            diff libtool.orig libtool
            echo "...done."
        fi
        echo "make > make.log 2>&1"
        make > make.log 2>&1
        if [ "$?" -ne "0" ] ; then
            echo "Error in " `pwd`
            exit 1
        fi
        echo "cd ${ODBC_SRC}"
        cd ${ODBC_SRC}
        echo "cd ${PSQL_ODBC}"
        cd ${PSQL_ODBC}
        #Handle repeated passes through this script cleanly
        if [ -f Makefile ]; then
            echo "make distclean > make-distclean.log 2>&1"
            make distclean > make-distclean.log 2>&1
        fi
        echo "./configure LDFLAGS=-L${ODBC_SRC}/${UNIX_ODBC}/odbcinst/.libs CPPFLAGS=-I${ODBC_SRC}/${UNIX_ODBC}/include prefix=${ODBC_DIR}/install > configure.log 2>&1"
        ./configure LDFLAGS=-L${ODBC_SRC}/${UNIX_ODBC}/odbcinst/.libs CPPFLAGS=-I${ODBC_SRC}/${UNIX_ODBC}/include prefix=${ODBC_DIR}/install > configure.log 2>&1
        if [ "$?" != "0" ] ; then
            echo "Error in " `pwd`
            exit 1
        fi

        #If the platform is OS X, add the ld flags to the Makefile
        #so psql odbc builds correctly
        if [ "${PLAT}" = "Darwin" ] ; then
            echo ""
            echo "Modifying the psqlodbc_la_LIBADD line in Makefile due to OS X bug"
            sed 's,^psqlodbc_la_LIBADD.*,psqlodbc_la_LIBADD = -lpq -lssl -lodbcinst,' Makefile > Makefile.tmp
            if [ $? -ne 0 ] ; then
                echo "Error editing Makefile"
                exit 1
            fi
            mv Makefile Makefile.orig
            mv Makefile.tmp Makefile
            diff Makefile.orig Makefile
            rm Makefile.orig
            echo "..done"
            echo ""
        fi

        echo "make > make.log 2>&1"
        make > make.log 2>&1
        if [ "$?" != "0" ] ; then
            echo "Error in " `pwd`
            exit 1
        fi
        echo "make install > make-install.log 2>&1"
        make install > make-install.log 2>&1
        if [ "$?" != "0" ] ; then
            echo "Error in " `pwd`
            exit 1
        fi
 
        echo "readme.txt -> ${ODBC_INSTALL}/readme.txt"
        cp -f readme.txt ${ODBC_INSTALL}/readme.txt
        echo "license.txt ${ODBC_INSTALL}/license.txt"
        cp -f license.txt ${ODBC_INSTALL}/license.txt
    
        echo
        echo "Done building ODBC component at" `date`
        echo
    fi
}

#compile GPDB
compileGPDB() {

    echo "Compiling GPDB at" `date`
    echo
    
    if [ "$WINDOWS" = "true" ] ; then
        #This is to get around postgresql not having a fully working 
        #crosscompile env
        #Change ./zic to zic, due to exe's not running in linux
        echo "Modifying timezone Makefile for crosscompile."
        perl -pi -e 's,./zic,zic,' cdb-pg/src/timezone/Makefile

        #Run a make -k, the selectively build some .exe's
        echo "Entering cdb-pg"
        cd cdb-pg

        echo "Setting PATH to include libcurl"
        echo "export PATH=/opt/curl-7.16.1-crosscompile/bin:${PATH}"
        export PATH=/opt/curl-7.16.1-crosscompile/bin:${PATH}

        echo "Setting PATH to include mingw32"
        echo "export PATH=/usr/local/mingw32/bin:/usr/local/mingw32/mingw32/bin:${PATH}"
        export PATH=/usr/local/mingw32/bin:/usr/local/mingw32/mingw32/bin:${PATH}

        echo "Running configure for win32 mingw env"
#        CFLAGS="-O0 -g" ./configure --build=x86_64-unknown-linux-gnu --host=i686-pc-mingw32 --without-zlib --enable-debug CC=/usr/local/mingw32/mingw32/../bin/mingw32-gcc CFLAGS="-O3 -funroll-loops -fomit-frame-pointer -fargument-noalias-global" LDFLAGS=-L/usr/local/mingw32/mingw32/lib CPP= CPPFLAGS=-I/usr/local/mingw32/mingw32/include  --with-CXX=/usr/local/mingw32/mingw32/../bin/mingw32-g++ --with-java  --enable-multibyte=UNICODE  --with-maxbackends=1024 --enable-integer-datetimes --with-pgport=5432 --prefix=${GPDB_INSTALL} > configure.out 2>&1
        CFLAGS="-O0 -g" ./configure --build=x86_64-unknown-linux-gnu --host=i686-pc-mingw32 --without-zlib --enable-debug CC=/usr/local/mingw32/bin/mingw32-gcc CFLAGS="-O3 -funroll-loops -fomit-frame-pointer -fargument-noalias-global" LDFLAGS=-L/usr/local/mingw32/mingw32/lib CPP= CPPFLAGS=-I/usr/local/mingw32/mingw32/include --enable-integer-datetimes --with-pgport=5432 --prefix=${GPDB_INSTALL}

        if [ $? -ne 0 ] ; then
            echo "Error: Problem with configure"
            exit 1
        fi
        echo "Running make -k > make.out 2>&1"
        make -k

        echo "Entering src/bin"
        cd src/bin
        #scripts
        echo "make -C scripts install > scripts-make.out 2>&1"
        make -C scripts install
        if [ $? -ne 0 ] ; then
            echo "Error"
            exit 1
        fi
        #psql
        echo "make -C psql > psql-make.out 2>&1"
        make -C psql
        if [ $? -ne 0 ] ; then
            echo "Error"
            exit 1
        fi
        echo "cp -p psql/psql.exe ${GPDB_INSTALL}/bin"
        cp -p psql/psql.exe ${GPDB_INSTALL}/bin
        if [ $? -ne 0 ] ; then
            echo "Error copying psql.exe"
            exit 1
        fi

        echo "make -C pg_config > pg_config-make.out 2>&1"
        make -C pg_config
        if [ $? -ne 0 ] ; then
            echo "Error"
            exit 1
        fi
        echo "cp -p pg_config/pg_config.exe -> ${GPDB_INSTALL}/bin/"
        cp -p pg_config/pg_config.exe ${GPDB_INSTALL}/bin/
        if [ $? -ne 0 ] ; then
            echo "Error"
            exit 1
        fi

        #Build the dump/restore utils
        echo "Entering pg_dump"
        cd pg_dump

        echo "make pg_dump > pg_dump-make.out 2>&1"
        make pg_dump
        if [ $? -ne 0 ] ; then
            echo "Error compiling pg_dump"
            exit 1
        fi
        echo "cp -p pg_dump${EXE_EXT} ${GPDB_INSTALL}/bin"
        cp -p pg_dump${EXE_EXT} ${GPDB_INSTALL}/bin
        if [ $? -ne 0 ] ; then
            echo "Error copying pg_dump"
            exit 1
        fi

        echo "make pg_dumpall > pg_dumpall-make.out 2>&1"
        make pg_dumpall
        if [ $? -ne 0 ] ; then
            echo "Error compiling pg_dumpall"
            exit 1
        fi
        echo "cp -p pg_dumpall${EXE_EXT} ${GPDB_INSTALL}/bin"
        cp -p pg_dumpall${EXE_EXT} ${GPDB_INSTALL}/bin
        if [ $? -ne 0 ] ; then
            echo "Error copying pg_dumpall"
            exit 1
        fi

        echo "make pg_restore > pg_restore-make.out 2>&1"
        make pg_restore
        if [ $? -ne 0 ] ; then
            echo "Error compiling pg_restore"
            exit 1
        fi
        echo "cp -p pg_restore${EXE_EXT} ${GPDB_INSTALL}/bin"
        cp -p pg_restore${EXE_EXT} ${GPDB_INSTALL}/bin
        if [ $? -ne 0 ] ; then
            echo "Error copying pg_dump"
            exit 1
        fi

	echo "Leaving pg_dump"
	cd ..

        #ecpg
        echo "cd ../interfaces/ecpg"
        cd ../interfaces/ecpg
        echo "make > make.out 2>&1"
        make
        echo "make install > install.out 2>&1"
        make install

        #Include files
        echo "cd ../../include"
        cd ../../include
        echo "make install > make.out 2>&1" 
        make install

        #install libpq
        echo "cd ../interfaces/libpq/"
        cd ../interfaces/libpq/
        echo "make install > make.out 2>&1"
        make install
	if [ $? -ne 0 ]; then
		cat gpfdist.out
		echo "Error building gpfdist"
		exit 1
	fi
	cd ..

	# package up gpfdist and gpload
	mkdir -p win32gpfdist.$$/bin/lib
	cp gpMgmt/bin/gpload win32gpfdist.$$/bin/gpload.py
	cp -r gpMgmt/bin/lib/pg8000 win32gpfdist.$$/bin/lib/
	cp -r gpMgmt/bin/lib/yaml win32gpfdist.$$/bin/lib/
	cp gpcc/build/bin/gpfdist.exe win32gpfdist.$$/bin/
	cd win32gpfdist.$$
	zip -r ../gpload-win32-0.9.zip bin
	cd ..
	rm -rf win32gpfdist.$$
    else
        echo "export GPROOT=${GPDB_INSTALL}"
        export GPROOT=${GPDB_INSTALL}
        if [ "$MPP_ARCH" = "SOL-x86_64" ]; then
            export PATH=/opt/python-2.5.1/bin:${PATH}
            export LD_LIBRARY_PATH=/opt/python-2.5.1/lib:$LD_LIBRARY_PATH
        fi
        echo "make authlibs > authlib-compile.out 2>&1"
        make authlibs > authlib-compile.out 2>&1
        if [ $? -ne 0 ] ; then
            echo "Error building auth libs"
            exit 1
        fi
        echo ". authlib_libpath_setup.sh"
        . authlib_libpath_setup.sh
        if [ $? -ne 0 ] ; then
            echo "Error sourcing auth lib path setup"
            exit 1
        fi
        echo "make dist AUTH=true > make-dist.out 2>&1"
        make dist > make-dist.out 2>&1
        if [ $? -ne 0 ] ; then
            echo "Error running make dist."
            exit 1 
        fi 
    fi
    echo
    echo "Done building GPDB at" `date`
    echo
}


#
#Install
#

installJDBC() {
    echo "${JDBC_DIR} -> ${JDBC_INSTALL_DIR}"
    mkdir -p ${JDBC_INSTALL_DIR}
    cp -p ${JDBC_DIR}/*.jar ${JDBC_INSTALL_DIR}
}

installODBC() {
    mkdir ${INSTALL_DIR}/drivers/odbc
    if [ $WINDOWS = true ] ; then
        echo "cd ${INSTALL_DIR}/drivers/odbc"
        cd ${INSTALL_DIR}/drivers/odbc
        echo "unzip ${ODBC_SRC}/psqlodbc-08_01_0200.zip"
        unzip ${ODBC_SRC}/psqlodbc-08_01_0200.zip
        echo "cd ${WORK_DIR}"
        cd ${WORK_DIR}
    else
        echo "${ODBC_INSTALL}/lib/* -> ${INSTALL_DIR}/drivers/odbc"
        cp ${ODBC_INSTALL}/lib/* ${INSTALL_DIR}/drivers/odbc

        #License and readme
        echo "${ODBC_INSTALL}/license.txt -> ${INSTALL_DIR}/drivers/odbc/LICENSE.txt"
        cp ${ODBC_INSTALL}/license.txt ${INSTALL_DIR}/drivers/odbc/LICENSE.txt
        echo "${ODBC_INSTALL}/readme.txt -> ${INSTALL_DIR}/drivers/odbc/README.txt"
        cp ${ODBC_INSTALL}/readme.txt ${INSTALL_DIR}/drivers/odbc/README.txt
        
        rm -f ${INSTALL_DIR}/drivers/odbc/*.la
    fi
}

installGPDB() {
    #Binaries
    if [ "${WINDOWS}" = "false" ] ; then
        GPDB_INSTALL=${GPDB_INSTALL}/greenplum-db-devel
    fi
    mkdir -p ${INSTALL_DIR}/bin
    GPDB_BIN_FILES="clusterdb createdb createlang createuser dropdb droplang dropuser pg_config pg_dump pg_restore psql vacuumdb ecpg reindexdb pg_dumpall"
    for file in ${GPDB_BIN_FILES}; do
        echo "${GPDB_INSTALL}/bin/${file}${EXE_EXT} -> ${INSTALL_DIR}/bin"
        cp -p ${GPDB_INSTALL}/bin/${file}${EXE_EXT} ${INSTALL_DIR}/bin
        if [ $? -ne 0 ] ; then
            echo "Error copying ${GPDB_INSTALL}/bin/${file}${EXE_EXT} -> ${INSTALL_DIR}/bin"
            exit 1
        fi
    done

    #libs
    mkdir -p ${LIB_INSTALL_DIR}
    echo "${GPDB_INSTALL}/lib/* -> ${LIB_INSTALL_DIR}"
    cp -pr ${GPDB_INSTALL}/lib/* ${LIB_INSTALL_DIR}

    #include
    mkdir -p ${INCLUDE_DIR}
    echo "${GPDB_INSTALL}/include/* -> ${INCLUDE_DIR}"
    cp -pr ${GPDB_INSTALL}/include/* ${INCLUDE_DIR}
}

installDOCS() {
    echo "${DOC_DIR}/LICENSE.txt -> ${INSTALL_DIR}/LICENSE.txt"
    cp ${DOC_DIR}/LICENSE.txt ${INSTALL_DIR}/LICENSE.txt
    if [ "${WINDOWS}" = "true" ] ; then
        echo "cp ${DOC_DIR}/GPClientToolsWin.pdf ${INSTALL_DIR}/GPClientTools.pdf"
        cp ${DOC_DIR}/GPClientToolsWin.pdf ${INSTALL_DIR}/GPClientTools.pdf
    else
        echo "${DOC_DIR}/README.txt -> ${INSTALL_DIR}/docs/README.txt"
        cp ${DOC_DIR}/README.txt ${INSTALL_DIR}/README.txt
    fi
}

installSCRIPTS() {
    echo "${SCRIPT_DIR}/greenplum_clients_path.${SCRIPT_EXT} -> ${INSTALL_DIR}"
    cp ${SCRIPT_DIR}/greenplum_clients_path.${SCRIPT_EXT} ${INSTALL_DIR}
    if [ $? -ne 0 ] ; then
        echo "Error: ${SCRIPT_DIR}/greenplum_clients_path.${SCRIPT_EXT} -> ${INSTALL_DIR}"
        exit 1
    fi 
}

installALL() {
    echo "Installing"
    rm -rf ${INSTALL_DIR}
    mkdir -p ${INSTALL_DIR}
    installJDBC
    installODBC
    installGPDB
    installSCRIPTS
    installDOCS
}

#Main
compileGPDB
compileODBC
installALL

echo ""
echo "**** DONE ****"
echo ""

