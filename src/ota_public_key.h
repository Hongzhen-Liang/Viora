#pragma once

// OTA RSA-3072 公钥。匹配的私钥只保存在服务器
// VioraServer/firmware/keys/ota_private.pem，不得烧录到设备或提交到公开仓库。
static const char OTA_SIGNING_PUBLIC_KEY[] = R"PEM(-----BEGIN PUBLIC KEY-----
MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAuZTINLH+V44P6oOaiGQX
wiBLpQUH0y5mTxJDbpe+OrLXI4osMLk/V7xs+yXJKvQP0vdQXzkRIUcUcJorDujQ
49EV3tK7k3hhO6Rcgsz7eYXqUZ97LqC76rp43kruyTI39gMq3iwUi5BjZHuLoiZA
v6lhI9aWsWjr3zTB01wa/hTuBJgHRF8dMV4FG7+qMjZRDj9RmgI1xVYrO2jkidCS
XkbiGLDT+WvKf+ObUesQaltkW2N7osWPOfb/oUCUqLySPjhY5hN20Z24nwvD0XFV
eeldQoSP7j2zxfGI/kRYl8tBXgmF3f3rw1my0zwfokhZ3Lw2zI+0YtolBsTFQ9x1
6bO2umVLTKD5UPWQP9URi42mDq9YTqGbTD7xZnuqJV4VbqUlrmnl243VMDP/Qyhz
d9sOTcTs5VCRm4e6anj18fMiHAR4RfcfaGXDjWrvFuY3Lcal4Ur3hAdT/AE+cmj/
OIINz9laOCx2DdvyNbHYBLMz44is5GzM9b5jfob1njr9AgMBAAE=
-----END PUBLIC KEY-----
)PEM";
