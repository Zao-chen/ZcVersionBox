#pragma once

#include <qglobal.h>

bool setMacAutoStartEnabled(bool enabled);
void setMacServicesProviderEnabled(bool enabled);
// WId is only declared by QtGui, which services TUs do not have on their
// include path; quintptr is WId's underlying type and comes with qglobal.h.
void setupMacTitleBar(quintptr winId);
