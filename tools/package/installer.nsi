; Instalador do Windows (NSIS). Nenhum dado do jogo vai no instalador; o disco do usuário é pedido na primeira execução.
Unicode true
!ifndef SRC
  !error "defina -DSRC=<pasta com dracula-x.exe, SDL3.dll e LEIAME.txt>"
!endif
!ifndef OUT
  !error "defina -DOUT=<arquivo .exe do instalador>"
!endif
Name "Dracula X PC"
OutFile "${OUT}"
InstallDir "$PROGRAMFILES64\Dracula X PC"
InstallDirRegKey HKLM "Software\DraculaXPC" "Dir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
Icon "icon.ico"
UninstallIcon "icon.ico"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Instalar"
  SetOutPath "$INSTDIR"
  File "${SRC}\dracula-x.exe"
  File "${SRC}\SDL3.dll"
  File "${SRC}\LEIAME.txt"
  File "icon.ico"
  WriteUninstaller "$INSTDIR\desinstalar.exe"
  CreateDirectory "$SMPROGRAMS\Dracula X PC"
  CreateShortcut "$SMPROGRAMS\Dracula X PC\Dracula X PC.lnk" "$INSTDIR\dracula-x.exe" "" "$INSTDIR\icon.ico"
  CreateShortcut "$SMPROGRAMS\Dracula X PC\Leia-me.lnk" "$INSTDIR\LEIAME.txt"
  CreateShortcut "$SMPROGRAMS\Dracula X PC\Desinstalar.lnk" "$INSTDIR\desinstalar.exe"
  WriteRegStr HKLM "Software\DraculaXPC" "Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DraculaXPC" "DisplayName" "Dracula X PC"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DraculaXPC" "UninstallString" '"$INSTDIR\desinstalar.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DraculaXPC" "DisplayIcon" "$INSTDIR\icon.ico"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DraculaXPC" "NoModify" 1
SectionEnd

; Os dados do usuário (pacote local, saves, configuração) ficam em %APPDATA%\dracula-x-pc e NÃO são apagados.
Section "Uninstall"
  Delete "$INSTDIR\dracula-x.exe"
  Delete "$INSTDIR\SDL3.dll"
  Delete "$INSTDIR\LEIAME.txt"
  Delete "$INSTDIR\icon.ico"
  Delete "$INSTDIR\desinstalar.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Dracula X PC\*.lnk"
  RMDir "$SMPROGRAMS\Dracula X PC"
  DeleteRegKey HKLM "Software\DraculaXPC"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DraculaXPC"
SectionEnd
