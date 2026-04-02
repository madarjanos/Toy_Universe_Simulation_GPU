@echo off
echo ********
echo Meg kell adnod a forditonak (gcc), hogy hol van a OpenCL-SDK mappa, amiben a h es lib fajl van!
echo Es persze a gcc legyen a PATH-ba, vagy add meg a teljes PATH-t hozza!
echo ********

gcc -O3 -march=native -ffast-math -std=c11 -o nbodysim.exe main.c nbody_simulation.c nbody_save.c render.c pngsave.c window.c -I C:\OpenCL-SDK\include -L C:\OpenCL-SDK\lib -lOpenCL -lgdi32 -luser32 -lgdiplus

pause
@echo on
