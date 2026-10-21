#pragma once
// RTC control registers do not exist on a host.
#define RTC_CNTL_OPTION1_REG 0
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 0
#define REG_WRITE(a, v) ((void)(a), (void)(v))
#define REG_READ(a) (0)
#define REG_SET_BIT(a, b) ((void)(a), (void)(b))
#define REG_CLR_BIT(a, b) ((void)(a), (void)(b))
