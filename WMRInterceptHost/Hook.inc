;WMRCamInterceptHost.cpp
EXTERN HookCloseCameraStream_RBPOffset:QWORD
EXTERN HookCloseCameraStream_StackSize:DWORD
EXTERN HookStartCameraStream_RSPSubOffs:DWORD

_OnOpenCameraStream PROTO FASTCALL
_OnCloseCameraStream PROTO FASTCALL
_OnStartCameraStream PROTO FASTCALL
_OnStopCameraStream PROTO FASTCALL

;WMRControllerInterceptHost.cpp
EXTERN HookCrystalKeyStartIMUStream_RSPSubOffs:DWORD
EXTERN HookCrystalKeyStopIMUStream_RSPSubOffs:DWORD
EXTERN HookCrystalKeyStopIMUStream_CMPAddr:QWORD
EXTERN HookControllerStateTransition_FormatString:QWORD
EXTERN HookControllerStateTransition_ModuleNameString:QWORD

_OnCrystalKeyStartIMUStream PROTO FASTCALL
_OnSuccessCrystalKeyStartIMUStream PROTO FASTCALL
_OnPostCrystalKeyStopIMUStream PROTO FASTCALL
_OnControllerStateTransition PROTO FASTCALL