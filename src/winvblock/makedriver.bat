@echo off

set lib=bus disk ramdisk filedisk

set links=
for /d %%a in (%lib%) do (
  pushd .
  cd %%a
  call makelib.bat
  popd
  )

set c=debug.c driver.c irp.c probe.c registry.c winvblock.rc device.c wv_stdlib.c wv_string.c

set name=WVBlk%bits%

echo !INCLUDE $(NTMAKEENV)\makefile.def	> makefile

echo INCLUDES=..\include		> sources
echo TARGETNAME=%name%			>> sources
echo TARGETTYPE=EXPORT_DRIVER		>> sources
echo TARGETPATH=obj			>> sources
echo LINKLIBS=%links%			>> sources
echo TARGETLIBS=%links%			>> sources
echo SOURCES=%c%			>> sources
echo C_DEFINES=-DPROJECT_BUS=1		>> sources

echo NAME %name%.sys			> %name%.def

build
copy obj%obj%\%arch%\%name%.sys ..\..\bin >nul
copy obj%obj%\%arch%\%name%.pdb ..\..\bin >nul
copy obj%obj%\%arch%\%name%.lib ..\..\bin >nul