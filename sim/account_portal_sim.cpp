// Portal de contas falso do simulador: so devolve URL e codigo de exemplo.

#include <stdio.h>

#include "account_portal.h"

static AccountPortalInfo info = {};

bool account_portal_start() {
  info.active = true;
  snprintf(info.url, sizeof(info.url), "http://192.168.15.129");
  snprintf(info.code, sizeof(info.code), "482913");
  return true;
}

void account_portal_stop() { info.active = false; }
AccountPortalInfo account_portal_info() { return info; }
void account_portal_loop() {}
