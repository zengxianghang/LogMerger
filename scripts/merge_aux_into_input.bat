@echo off
setlocal

if "%~1"=="" (
  set INPUT=input.log
) else (
  set INPUT=%~1
)

if "%~2"=="" (
  set AUX=aux.log
) else (
  set AUX=%~2
)

if "%~3"=="" (
  set OUTPUT=input_merge.log
) else (
  set OUTPUT=%~3
)

merge_aux_into_input.exe %INPUT% %AUX% %OUTPUT%

endlocal
