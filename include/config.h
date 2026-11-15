#pragma once

#if defined(INGEST_ENV_LOCAL)
#elif defined(INGEST_ENV_PROD)
#include "config_prod.h"
#else
#error "Build with INGEST_ENV_LOCAL or INGEST_ENV_PROD"
#endif
