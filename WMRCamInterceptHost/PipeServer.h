#pragma once

bool RunCamServer();
void CloseCamServer();

void OnStartCameraStream(WORD id, unsigned char count, unsigned short sizeX, unsigned short sizeY);
void OnGetStreamImage(WORD id, const BYTE *buf, unsigned char count, unsigned short sizeX, unsigned short sizeY);
void OnGetStreamImageDefault(WORD id, const BYTE *imageBuf);
void OnStopCameraStream(WORD id);
void OnErrorLog(const char *error);