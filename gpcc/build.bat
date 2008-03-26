@echo off

set GPCC=%CD%
set EXT=%GPCC%\ext
set APR128=%GPCC%\ext\apr-1.2.8
set APU128=%GPCC%\ext\apr-util-1.2.8
set APR=%GPCC%\ext\apr
set APU=%GPCC%\ext\apr-util
set JSON=%GPCC%\ext\json-c-0.6
set LIBEVENT=%GPCC%\ext\libevent-1.3e
set LIBGPCC=%GPCC%\WIN32\libgpcc
set GPFDIST=%GPCC%\WIN32\gpfdist
set ZLIB=%GPCC%\ext\zlib-1.2.3

cd %EXT%

if not exist apr (
	%GPCC%\WIN32\7za x apr-1.2.8.tar.gz -so > apr-1.2.8.tar
	%GPCC%\WIN32\7za x apr-1.2.8.tar
	rename apr-1.2.8 apr
	del apr-1.2.8.tar
)

if not exist apr-util (
	%GPCC%\WIN32\7za x apr-util-1.2.8.tar.gz -so > apr-util-1.2.8.tar
	%GPCC%\WIN32\7za x apr-util-1.2.8.tar
	rename apr-util-1.2.8 apr-util
	del apr-util-1.2.8.tar
)

if not exist json-c-0.6 (
	%GPCC%\WIN32\7za x json-c-0.6.tar.gz -so > json-c-0.6.tar
	%GPCC%\WIN32\7za x json-c-0.6.tar
	del json-c-0.6.tar
)

if not exist libevent-1.3e (
	%GPCC%\WIN32\7za x libevent-1.3e.tar.gz -so > libevent-1.3e.tar
	%GPCC%\WIN32\7za x libevent-1.3e.tar
	del libevent-1.3e.tar
)

if not exist zlib-1.2.3 (
	%GPCC%\WIN32\7za x zlib-1.2.3.zip -r
)

cd %GPCC%

copy %GPCC%\ext\WIN32\apr\*.vcproj %APR%
copy %GPCC%\ext\WIN32\apr\*.sln %APR%
copy %GPCC%\ext\WIN32\apr\*.hw %APR%\include

copy %GPCC%\ext\WIN32\apr-util\aprutil.vcproj %APU%
copy %GPCC%\ext\WIN32\apr-util\xml\expat\lib\xml.vcproj %APU%\xml\expat\lib
copy %GPCC%\ext\WIN32\apr-util\*.sln %APU%
copy %GPCC%\ext\WIN32\apr-util\*.hw %APU%\include

copy %GPCC%\ext\WIN32\json\*.* %JSON%
copy %JSON%\config.h.win32 %JSON%\config.h

copy %GPCC%\ext\WIN32\libevent\*.* %LIBEVENT%\WIN32-Prj


cd %EXT%

echo ---------- build apr and apr-util ----------------------
rem rename apr-1.2.2 to apr
rem rename apr-util-1.2.2 to apr-util
rem 1. cd into ext/apr-util and open aprutil.dsw using visual studio
rem 
rem 2. open aprutil | Generated Files | apu.hw
rem 3. edit ext/apr-util/include/apu.hw
rem    add line: #define HAVE_ICONV_H           1
rem      
rem    redefine: #define APU_HAVE_APR_ICONV     0
rem              #define APU_HAVE_ICONV         0
rem  4. edit ext/apr-util/include/apr_ldap.hw
rem     redefine: #define APR_HAS_LDAP	0



cd %APR%

vcbuild /u apr.vcproj /clean
vcbuild /u apr.vcproj "Release|Win32"

cd %APU%\xml\expat\lib

vcbuild /u xml.vcproj /clean
vcbuild /u xml.vcproj "Release|Win32"

cd %APU%

vcbuild /u aprutil.vcproj /clean
vcbuild /u aprutil.vcproj "Release|Win32"


echo ---------------------- json ----------------------------
cd %JSON%

vcbuild /u json-c.vcproj /clean
vcbuild /u json-c.vcproj "Release|Win32"

echo ---------------------- libevent ----------------------------
cd %LIBEVENT%\WIN32-Prj

vcbuild /u libevent.vcproj /clean
vcbuild /u libevent.vcproj "Release|Win32"

echo --------------------- zlib ---------------------------------
cd %ZLIB%\projects\vc2005

vcbuild /u zlib.vcproj /clean
vcbuild /u zlib.vcproj "LIB Release|Win32"

echo --------------------- libgpcc ------------------------------
cd %LIBGPCC%

vcbuild /u libgpcc.vcproj /clean
vcbuild /u libgpcc.vcproj "Release|Win32"

echo --------------------- gpfdist ------------------------------
cd %GPFDIST%

vcbuild /u gpfdist.vcproj /clean
vcbuild /u gpfdist.vcproj "Release|Win32"

cd %GPCC%
