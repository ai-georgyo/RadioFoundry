// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ECDSA P-256 (core/crypto/p256.c, docs/spec/p256.md) against every known-answer set of p256.md 12, which names each
// one's source: k G (P256-MULT, CAVP KeyPair), the field product past 4p (P256-FIELD), RFC 6979 A.2.5, CAVP SigGen
// with the given k, SigVer, PKV, the derived edge cases (P256-EDGE) and M17's own signature (M17-SIG: the high s
// accepted, s + 1 refused, n - s accepted). Plus what the header promises: G's table recomputed from G, compression
// round trips, invalid keys and points refused, the scratch wiped after every secret operation (success or not), the
// same number of field multiplies for keys of any weight, a scratch at any 4-byte alignment. The file includes p256.c
// itself, so it reaches the field and point code; built plain and with -DFXP_SPLIT16 (the C-SKY multiply).
// `p256Test bench` times sign and verify (meaningful in an optimized build only).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define P256_TEST_COUNT 1
unsigned long p256_test_muls;
#include "crypto/p256.c"
#include "support/sha256.h"

static unsigned checks, failed;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failed++;
		printf("FAIL %s\n", what);
	}
}

// Hex text to bytes, right-aligned in out[n] (a shorter value gets leading zeros); returns the digits' byte count,
// so a value longer than n is seen by the caller
static size_t hex(const char *s, uint8_t *out, size_t n)
{
	size_t digits = strlen(s), bytes = (digits + 1) / 2;
	uint8_t tmp[128];

	if (bytes > sizeof tmp) {
		printf("FAIL vector too long\n");
		failed++;
		return bytes;
	}
	memset(tmp, 0, sizeof tmp);
	for (size_t i = 0; i < digits; i++) {
		char ch = s[digits - 1 - i];
		unsigned v = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0') : ch >= 'a' && ch <= 'f' ?
			(unsigned)(ch - 'a' + 10) : (unsigned)(ch - 'A' + 10);
		tmp[sizeof tmp - 1 - i / 2] |= (uint8_t)(v << (4 * (i & 1)));
	}
	memset(out, 0, n);
	memcpy(out + (n > bytes ? n - bytes : 0), tmp + sizeof tmp - (n < bytes ? n : bytes), n < bytes ? n : bytes);
	return bytes;
}

static void hex2(const char *x, const char *y, uint8_t out[64])
{
	hex(x, out, 32);
	hex(y, out + 32, 32);
}

static int eq_hex(const uint8_t *got, size_t n, const char *want)
{
	uint8_t w[64];

	hex(want, w, n);
	return memcmp(got, w, n) == 0;
}

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	for (size_t i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
}

// The scratch every test lends, and one at an address that is 4 but not 8 aligned (the context rounds up)
static union {
	p256_scratch_t s;
	uint64_t align;
	uint8_t b[sizeof(p256_scratch_t) + 8];
} scr;
static p256_scratch_t *S = &scr.s;
static p256_scratch_t *S4 = (p256_scratch_t *)(scr.b + 4);

static void sha(const void *p, size_t n, uint8_t out[32])
{
	sha256_t c;

	sha256_init(&c);
	sha256_update(&c, p, n);
	sha256_final(&c, out);
}

// ---- Known answers (each set's public source in p256.md 12; values in hex, big endian) ----

// P256-MULT: Q = k G for k = 1 to 5, 7, a 57-bit k, n - 3 to n - 1 and (2^256 - 1) mod n (derived from the domain
// parameters of p256.md 2, each checked with OpenSSL: p256.md 12)
static const struct { const char *k, *qx, *qy; } mult[] = {
	{ "0000000000000000000000000000000000000000000000000000000000000001",
	  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
	  "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" },
	{ "0000000000000000000000000000000000000000000000000000000000000002",
	  "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978",
	  "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1" },
	{ "0000000000000000000000000000000000000000000000000000000000000003",
	  "5ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c",
	  "8734640c4998ff7e374b06ce1a64a2ecd82ab036384fb83d9a79b127a27d5032" },
	{ "0000000000000000000000000000000000000000000000000000000000000004",
	  "e2534a3532d08fbba02dde659ee62bd0031fe2db785596ef509302446b030852",
	  "e0f1575a4c633cc719dfee5fda862d764efc96c3f30ee0055c42c23f184ed8c6" },
	{ "0000000000000000000000000000000000000000000000000000000000000005",
	  "51590b7a515140d2d784c85608668fdfef8c82fd1f5be52421554a0dc3d033ed",
	  "e0c17da8904a727d8ae1bf36bf8a79260d012f00d4d80888d1d0bb44fda16da4" },
	{ "0000000000000000000000000000000000000000000000000000000000000007",
	  "8e533b6fa0bf7b4625bb30667c01fb607ef9f8b8a80fef5b300628703187b2a3",
	  "73eb1dbde03318366d069f83a6f5900053c73633cb041b21c55e1a86c1f400b4" },
	{ "000000000000000000000000000000000000000000000000018ebbb95eed0e13",
	  "339150844ec15234807fe862a86be77977dbfb3ae3d96f4c22795513aeaab82f",
	  "b1c14ddfdc8ec1b2583f51e85a5eb3a155840f2034730e9b5ada38b674336a21" },
	{ "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254e",
	  "5ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c",
	  "78cb9bf2b6670082c8b4f931e59b5d1327d54fcac7b047c265864ed85d82afcd" },
	{ "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254f",
	  "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978",
	  "f888aaee24712fc0d6c26539608bcf244582521ac3167dd661fb4862dd878c2e" },
	{ "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550",
	  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
	  "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a" },
	{ "00000000ffffffff00000000000000004319055258e8617b0c46353d039cdaae",
	  "f72cbd240e26c0d21b1023179586eb532c6102c49c3677cc1a3d132b9db9d31a",
	  "43e4ca77e2a36621dc0dbd91bfe7a5d223250ef0cdca831ee453d93fa83408a7" },
};

// P256-FIELD: x y mod p and x y 2^-256 mod p, a product past 4p in the NIST reduction (computed: p256.md 12)
static const char *field_x = "ffffffff00000001000000000000000000000000fffffffffffffffffffffffe", *field_y = "fffffffefffffffdffffffff0000000100000001000000010000000300000004",
	*field_xy = "000000000000000300000000fffffffefffffffffffffffefffffffcfffffffb",
	*field_mont = "fffffffcfffffffc00000004fffffff4fffffffffffffffdfffffff1fffffffc";

// RFC6979-P256-SHA256 (A.2.5): the key, then per message H, k, r, s
static const char *rfc_d = "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721",
	*rfc_qx = "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6",
	*rfc_qy = "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299";
static const struct { const char *msg, *h, *k, *r, *s; } rfc[] = {
	{ "sample", "af2bdbe1aa9b6ec1e2ade1d694f41fc71a831d0268e9891562113d8a62add1bf",
	  "a6e3c57dd01abe90086538398355dd4c3b17aa873382b0f24d6129493d8aad60",
	  "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716",
	  "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8" },
	{ "test", "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
	  "d16b6ae827f17175e040871a1c7ec3500192c4c92677336ec2537acaee0008e0",
	  "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367",
	  "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083" },
};

// CAVP SigGen [P-256,SHA-256], H = SHA-256(Msg): a known k
static const struct { const char *h, *d, *qx, *qy, *k, *r, *s; } siggen[] = {
	{ "44acf6b7e36c1342c2c5897204fe09504e1e2efb1a900377dbc4e7a6a133ec56",
	  "519b423d715f8b581f4fa8ee59f4771a5b44c8130b4e3eacca54a56dda72b464",
	  "1ccbe91c075fc7f4f033bfa248db8fccd3565de94bbfb12f3c59ff46c271bf83",
	  "ce4014c68811f9a21a1fdb2c0e6113e06db7ca93b7404e78dc7ccd5ca89a4ca9",
	  "94a1bbb14b906a61a280f245f9e93c7f3b4a6247824f5d33b9670787642a68de",
	  "f3ac8061b514795b8843e3d6629527ed2afd6b1f6a555a7acabb5e6f79c8c2ac",
	  "8bf77819ca05a6b2786c76262bf7371cef97b218e96f175a3ccdda2acc058903" },
	{ "9b2db89cb0e8fa3cc7608b4d6cc1dec0114e0b9ff4080bea12b134f489ab2bbc",
	  "0f56db78ca460b055c500064824bed999a25aaf48ebb519ac201537b85479813",
	  "e266ddfdc12668db30d4ca3e8f7749432c416044f2d2b8c10bf3d4012aeffa8a",
	  "bfa86404a2e9ffe67d47c587ef7a97a7f456b863b4d02cfc6928973ab5b1cb39",
	  "6d3e71882c3b83b156bb14e0ab184aa9fb728068d3ae9fac421187ae0b2f34c6",
	  "976d3a4e9d23326dc0baa9fa560b7c4e53f42864f508483a6473b6a11079b2db",
	  "1b766e9ceb71ba6c01dcd46e0af462cd4cfa652ae5017d4555b8eeefe36e1932" },
	{ "b804cf88af0c2eff8bbbfb3660ebb3294138e9d3ebd458884e19818061dacff0",
	  "e283871239837e13b95f789e6e1af63bf61c918c992e62bca040d64cad1fc2ef",
	  "74ccd8a62fba0e667c50929a53f78c21b8ff0c3c737b0b40b1750b2302b0bde8",
	  "29074e21f3a0ef88b9efdf10d06aa4c295cc1671f758ca0e4cd108803d0f2614",
	  "ad5e887eb2b380b8d8280ad6e5ff8a60f4d26243e0124c2f31a297b5d0835de2",
	  "35fb60f5ca0f3ca08542fb3cc641c8263a2cab7a90ee6a5e1583fac2bb6f6bd1",
	  "ee59d81bc9db1055cc0ed97b159d8784af04e98511d0a9a407b99bb292572e96" },
	{ "85b957d92766235e7c880ac5447cfbe97f3cb499f486d1e43bcb5c2ff9608a1a",
	  "a3d2d3b7596f6592ce98b4bfe10d41837f10027a90d7bb75349490018cf72d07",
	  "322f80371bf6e044bc49391d97c1714ab87f990b949bc178cb7c43b7c22d89e1",
	  "3c15d54a5cc6b9f09de8457e873eb3deb1fceb54b0b295da6050294fae7fd999",
	  "24fc90e1da13f17ef9fe84cc96b9471ed1aaac17e3a4bae33a115df4e5834f18",
	  "d7c562370af617b581c84a2468cc8bd50bb1cbf322de41b7887ce07c0e5884ca",
	  "b46d9f2d8c4bf83546ff178f1d78937c008d64e8ecc5cbb825cb21d94d670d89" },
	{ "3360d699222f21840827cf698d7cb635bee57dc80cd7733b682d41b55b666e22",
	  "53a0e8a8fe93db01e7ae94e1a9882a102ebd079b3a535827d583626c272d280d",
	  "1bcec4570e1ec2436596b8ded58f60c3b1ebc6a403bc5543040ba82963057244",
	  "8af62a4c683f096b28558320737bf83b9959a46ad2521004ef74cf85e67494e1",
	  "5d833e8d24cc7a402d7ee7ec852a3587cddeb48358cea71b0bedb8fabe84e0c4",
	  "18caaf7b663507a8bcd992b836dec9dc5703c080af5e51dfa3a9a7c387182604",
	  "77c68928ac3b88d985fb43fb615fb7ff45c18ba5c81af796c613dfa98352d29c" },
	{ "c413c4908cd0bc6d8e32001aa103043b2cf5be7fcbd61a5cec9488c3a577ca57",
	  "4af107e8e2194c830ffb712a65511bc9186a133007855b49ab4b3833aefc4a1d",
	  "a32e50be3dae2c8ba3f5e4bdae14cf7645420d425ead94036c22dd6c4fc59e00",
	  "d623bf641160c289d6742c6257ae6ba574446dd1d0e74db3aaa80900b78d4ae9",
	  "e18f96f84dfa2fd3cdfaec9159d4c338cd54ad314134f0b31e20591fc238d0ab",
	  "8524c5024e2d9a73bde8c72d9129f57873bbad0ed05215a372a84fdbc78f2e68",
	  "d18c2caf3b1072f87064ec5e8953f51301cada03469c640244760328eb5a05cb" },
	{ "88fc1e7d849794fc51b135fa135deec0db02b86c3cd8cebdaa79e8689e5b2898",
	  "78dfaa09f1076850b3e206e477494cddcfb822aaa0128475053592c48ebaf4ab",
	  "8bcfe2a721ca6d753968f564ec4315be4857e28bef1908f61a366b1f03c97479",
	  "0f67576a30b8e20d4232d8530b52fb4c89cbc589ede291e499ddd15fe870ab96",
	  "295544dbb2da3da170741c9b2c6551d40af7ed4e891445f11a02b66a5c258a77",
	  "c5a186d72df452015480f7f338970bfe825087f05c0088d95305f87aacc9b254",
	  "84a58f9e9d9e735344b316b1aa1ab5185665b85147dc82d92e969d7bee31ca30" },
	{ "41fa8d8b4cd0a5fdf021f4e4829d6d1e996bab6b4a19dcb85585fe76c582d2bc",
	  "80e692e3eb9fcd8c7d44e7de9f7a5952686407f90025a1d87e52c7096a62618a",
	  "a88bc8430279c8c0400a77d751f26c0abc93e5de4ad9a4166357952fe041e767",
	  "2d365a1eef25ead579cc9a069b6abc1b16b81c35f18785ce26a10ba6d1381185",
	  "7c80fd66d62cc076cef2d030c17c0a69c99611549cb32c4ff662475adbe84b22",
	  "9d0c6afb6df3bced455b459cc21387e14929392664bb8741a3693a1795ca6902",
	  "d7f9ddd191f1f412869429209ee3814c75c72fa46a9cccf804a2f5cc0b7e739f" },
	{ "2d72947c1731543b3d62490866a893952736757746d9bae13e719079299ae192",
	  "5e666c0db0214c3b627a8e48541cc84a8b6fd15f300da4dff5d18aec6c55b881",
	  "1bc487570f040dc94196c9befe8ab2b6de77208b1f38bdaae28f9645c4d2bc3a",
	  "ec81602abd8345e71867c8210313737865b8aa186851e1b48eaca140320f5d8f",
	  "2e7625a48874d86c9e467f890aaa7cd6ebdf71c0102bfdcfa24565d6af3fdce9",
	  "2f9e2b4e9f747c657f705bffd124ee178bbc5391c86d056717b140c153570fd9",
	  "f5413bfd85949da8d83de83ab0d19b2986613e224d1901d76919de23ccd03199" },
	{ "e138bd577c3729d0e24a98a82478bcc7482499c4cdf734a874f7208ddbc3c116",
	  "f73f455271c877c4d5334627e37c278f68d143014b0a05aa62f308b2101c5308",
	  "b8188bd68701fc396dab53125d4d28ea33a91daf6d21485f4770f6ea8c565dde",
	  "423f058810f277f8fe076f6db56e9285a1bf2c2a1dae145095edd9c04970bc4a",
	  "62f8665fd6e26b3fa069e85281777a9b1f0dfd2c0b9f54a086d0c109ff9fd615",
	  "1cc628533d0004b2b20e7f4baad0b8bb5e0673db159bbccf92491aef61fc9620",
	  "880e0bbf82a8cf818ed46ba03cf0fc6c898e36fca36cc7fdb1d2db7503634430" },
	{ "17b03f9f00f6692ccdde485fc63c4530751ef35da6f71336610944b0894fcfb8",
	  "b20d705d9bd7c2b8dc60393a5357f632990e599a0975573ac67fd89b49187906",
	  "51f99d2d52d4a6e734484a018b7ca2f895c2929b6754a3a03224d07ae61166ce",
	  "4737da963c6ef7247fb88d19f9b0c667cac7fe12837fdab88c66f10d3c14cad1",
	  "72b656f6b35b9ccbc712c9f1f3b1a14cbbebaec41c4bca8da18f492a062d6f6f",
	  "9886ae46c1415c3bc959e82b760ad760aab66885a84e620aa339fdf102465c42",
	  "2bf3a80bc04faa35ebecc0f4864ac02d349f6f126e0f988501b8d3075409a26c" },
	{ "c25beae638ff8dcd370e03a6f89c594c55bed1277ee14d83bbb0ef783a0517c7",
	  "d4234bebfbc821050341a37e1240efe5e33763cbbb2ef76a1c79e24724e5a5e7",
	  "8fb287f0202ad57ae841aea35f29b2e1d53e196d0ddd9aec24813d64c0922fb7",
	  "1f6daff1aa2dd2d6d3741623eecb5e7b612997a1039aab2e5cf2de969cfea573",
	  "d926fe10f1bfd9855610f4f5a3d666b1a149344057e35537373372ead8b1a778",
	  "490efd106be11fc365c7467eb89b8d39e15d65175356775deab211163c2504cb",
	  "644300fc0da4d40fb8c6ead510d14f0bd4e1321a469e9c0a581464c7186b7aa7" },
	{ "5eb28029ebf3c7025ff2fc2f6de6f62aecf6a72139e1cba5f20d11bbef036a7f",
	  "b58f5211dff440626bb56d0ad483193d606cf21f36d9830543327292f4d25d8c",
	  "68229b48c2fe19d3db034e4c15077eb7471a66031f28a980821873915298ba76",
	  "303e8ee3742a893f78b810991da697083dd8f11128c47651c27a56740a80c24c",
	  "e158bf4a2d19a99149d9cdb879294ccb7aaeae03d75ddd616ef8ae51a6dc1071",
	  "e67a9717ccf96841489d6541f4f6adb12d17b59a6bef847b6183b8fcf16a32eb",
	  "9ae6ba6d637706849a6a9fc388cf0232d85c26ea0d1fe7437adb48de58364333" },
	{ "12135386c09e0bf6fd5c454a95bcfe9b3edb25c71e455c73a212405694b29002",
	  "54c066711cdb061eda07e5275f7e95a9962c6764b84f6f1f3ab5a588e0a2afb1",
	  "0a7dbb8bf50cb605eb2268b081f26d6b08e012f952c4b70a5a1e6e7d46af98bb",
	  "f26dd7d799930062480849962ccf5004edcfd307c044f4e8f667c9baa834eeae",
	  "646fe933e96c3b8f9f507498e907fdd201f08478d0202c752a7c2cfebf4d061a",
	  "b53ce4da1aa7c0dc77a1896ab716b921499aed78df725b1504aba1597ba0c64b",
	  "d7c246dc7ad0e67700c373edcfdd1c0a0495fc954549ad579df6ed1438840851" },
	{ "aea3e069e03c0ff4d6b3fa2235e0053bbedc4c7e40efbc686d4dfb5efba4cfed",
	  "34fa4682bf6cb5b16783adcd18f0e6879b92185f76d7c920409f904f522db4b1",
	  "105d22d9c626520faca13e7ced382dcbe93498315f00cc0ac39c4821d0d73737",
	  "6c47f3cbbfa97dfcebe16270b8c7d5d3a5900b888c42520d751e8faf3b401ef4",
	  "a6f463ee72c9492bc792fe98163112837aebd07bab7a84aaed05be64db3086f4",
	  "542c40a18140a6266d6f0286e24e9a7bad7650e72ef0e2131e629c076d962663",
	  "4f7f65305e24a6bbb5cff714ba8f5a2cee5bdc89ba8d75dcbf21966ce38eb66f" },
};

// CAVP SigVer [P-256,SHA-256], H = SHA-256(Msg)
static const struct { const char *h, *qx, *qy, *r, *s; bool ok; const char *why; } sigver[] = {
	{ "a82c31412f537135d1c418bd7136fb5fde9426e70c70e7c2fb11f02f30fdeae2",
	  "87f8f2b218f49845f6f10eec3877136269f5c1a54736dbdf69f89940cad41555",
	  "e15f369036f49842fac7a86c8a2b0557609776814448b8f5e84aa9f4395205e9",
	  "d19ff48b324915576416097d2544f7cbdf8768b1454ad20e0baac50e211f23b0",
	  "a3e81e59311cdfff2d4784949f7a2cb50ba6c3a91fa54710568e61aca3e847c6", false, "F (3 - S changed)" },
	{ "5984eab8854d0a9aa5f0c70f96deeb510e5f9ff8c51befcdc3c41bac53577f22",
	  "5cf02a00d205bdfee2016f7421807fc38ae69e6b7ccd064ee689fc1a94a9f7d2",
	  "ec530ce3cc5c9d1af463f264d685afe2b4db4b5828d7e61b748930f3ce622a85",
	  "dc23d130c6117fb5751201455e99f36f59aba1a6a21cf2d0e7481a97451d6693",
	  "d6ce7708c18dbf35d4f8aa7240922dc6823f2e7058cbc1484fcad1599db5018c", false, "F (2 - R changed)" },
	{ "44b02ad3088076f997220a68ff0b27a58ecfa528b604427097cce5ca956274c5",
	  "2ddfd145767883ffbb0ac003ab4a44346d08fa2570b3120dcce94562422244cb",
	  "5f70c7d11ac2b7a435ccfbbae02c3df1ea6b532cc0e9db74f93fffca7c6f9a64",
	  "9913111cff6f20c5bf453a99cd2c2019a4e749a49724a08774d14e4c113edda8",
	  "9467cd4cd21ecb56b0cab0a9a453b43386845459127a952421f5c6382866c5cc", false, "F (4 - Q changed)" },
	{ "d1b8ef21eb4182ee270638061063a3f3c16c114e33937f69fb232cc833965a94",
	  "e424dc61d4bb3cb7ef4344a7f8957a0c5134e16f7a67c074f82e6e12f49abf3c",
	  "970eed7aa2bc48651545949de1dddaf0127e5965ac85d1243d6f60e7dfaee927",
	  "bf96b99aa49c705c910be33142017c642ff540c76349b9dab72f981fd9347f4f",
	  "17c55095819089c2e03b9cd415abdf12444e323075d98f31920b9e0f57ec871c", true, "P (0 )" },
	{ "b9336a8d1f3e8ede001d19f41320bc7672d772a3d2cb0e435fff3c27d6804a2c",
	  "e0fc6a6f50e1c57475673ee54e3a57f9a49f3328e743bf52f335e3eeaa3d2864",
	  "7f59d689c91e463607d9194d99faf316e25432870816dde63f5d4b373f12f22a",
	  "1d75830cd36f4c9aa181b2c4221e87f176b7f05b7c87824e82e396c88315c407",
	  "cb2acb01dac96efc53a32d4a0d85d0c2e48955214783ecf50a4f0414a319c05a", true, "P (0 )" },
	{ "640c13e290147a48c83e0ea75a0f92723cda125ee21a747e34c8d1b36f16cf2d",
	  "a849bef575cac3c6920fbce675c3b787136209f855de19ffe2e8d29b31a5ad86",
	  "bf5fe4f7858f9b805bd8dcc05ad5e7fb889de2f822f3d8b41694e6c55c16b471",
	  "25acc3aa9d9e84c7abf08f73fa4195acc506491d6fc37cb9074528a7db87b9d6",
	  "9b21d5b5259ed3f2ef07dfec6cc90d3a37855d1ce122a85ba6a333f307d31537", false, "F (2 - R changed)" },
	{ "8a3e7ad7b9b1b0cdc48e58d1e651fe6d710fef1420addeb61582bdd982d2b44c",
	  "3dfb6f40f2471b29b77fdccba72d37c21bba019efa40c1c8f91ec405d7dcc5df",
	  "f22f953f1e395a52ead7f3ae3fc47451b438117b1e04d613bc8555b7d6e6d1bb",
	  "548886278e5ec26bed811dbb72db1e154b6f17be70deb1b210107decb1ec2a5a",
	  "e93bfebd2f14f3d827ca32b464be6e69187f5edbd52def4f96599c37d58eee75", false, "F (4 - Q changed)" },
	{ "d80e9933e86769731ec16ff31e6821531bcf07fcbad9e2ac16ec9e6cb343a870",
	  "69b7667056e1e11d6caf6e45643f8b21e7a4bebda463c7fdbc13bc98efbd0214",
	  "d3f9b12eb46c7c6fda0da3fc85bc1fd831557f9abc902a3be3cb3e8be7d1aa2f",
	  "288f7a1cd391842cce21f00e6f15471c04dc182fe4b14d92dc18910879799790",
	  "247b3c4e89a3bcadfea73c7bfd361def43715fa382b8c3edf4ae15d6e55e9979", false, "F (1 - Message changed)" },
	{ "7c1048884558961c7e178b3a9b22583fca0d17f355a9887e2f96d363d2a776a3",
	  "bf02cbcf6d8cc26e91766d8af0b164fc5968535e84c158eb3bc4e2d79c3cc682",
	  "069ba6cb06b49d60812066afa16ecf7b51352f2c03bd93ec220822b1f3dfba03",
	  "f5acb06c59c2b4927fb852faa07faf4b1852bbb5d06840935e849c4d293d1bad",
	  "049dab79c89cc02f1484c437f523e080a75f134917fda752f2d5ca397addfe5d", false, "F (3 - S changed)" },
	{ "4c8d1afb724ad0c2ec458d866ac1dbb4497e273bbf05f88153102987e376fa75",
	  "224a4d65b958f6d6afb2904863efd2a734b31798884801fcab5a590f4d6da9de",
	  "178d51fddada62806f097aa615d33b8f2404e6b1479f5fd4859d595734d6d2b9",
	  "87b93ee2fecfda54deb8dff8e426f3c72c8864991f8ec2b3205bb3b416de93d2",
	  "4044a24df85be0cc76f21a4430b75b8e77b932a87f51e4eccbc45c263ebf8f66", false, "F (2 - R changed)" },
	{ "8581034ec7d7a6b163d71820923f616b362748f2846042c9896d8e4bf7577960",
	  "43691c7795a57ead8c5c68536fe934538d46f12889680a9cb6d055a066228369",
	  "f8790110b3c3b281aa1eae037d4f1234aff587d903d93ba3af225c27ddc9ccac",
	  "8acd62e8c262fa50dd9840480969f4ef70f218ebf8ef9584f199031132c6b1ce",
	  "cfca7ed3d4347fb2a29e526b43c348ae1ce6c60d44f3191b6d8ea3a2d9c92154", false, "F (3 - S changed)" },
	{ "e5b30e0041a33281210644938d9aaa15ef2c1247b4178f7ca1ee935ce23daabc",
	  "9157dbfcf8cf385f5bb1568ad5c6e2a8652ba6dfc63bc1753edf5268cb7eb596",
	  "972570f4313d47fc96f7c02d5594d77d46f91e949808825b3d31f029e8296405",
	  "dfaea6f297fa320b707866125c2a7d5d515b51a503bee817de9faa343cc48eeb",
	  "8f780ad713f9c3e5a4f7fa4c519833dfefc6a7432389b1e4af463961f09764f2", false, "F (1 - Message changed)" },
	{ "edd72dc0aa91649e09e2489c37ec27efab3b61953762c6b4532a9b1cd08a500d",
	  "072b10c081a4c1713a294f248aef850e297991aca47fa96a7470abe3b8acfdda",
	  "9581145cca04a0fb94cedce752c8f0370861916d2a94e7c647c5373ce6a4c8f5",
	  "09f5483eccec80f9d104815a1be9cc1a8e5b12b6eb482a65c6907b7480cf4f19",
	  "a4f90e560c5e4eb8696cb276e5165b6a9d486345dedfb094a76e8442d026378d", false, "F (4 - Q changed)" },
	{ "0d06ba42d256062e16b319a0f3099109518a765f26bac3b9f56930d965617726",
	  "09308ea5bfad6e5adf408634b3d5ce9240d35442f7fe116452aaec0d25be8c24",
	  "f40c93e023ef494b1c3079b2d10ef67f3170740495ce2cc57f8ee4b0618b8ee5",
	  "5cc8aa7c35743ec0c23dde88dabd5e4fcd0192d2116f6926fef788cddb754e73",
	  "9c9c045ebaa1b828c32f82ace0d18daebf5e156eb7cbfdc1eff4399a8a900ae7", false, "F (1 - Message changed)" },
	{ "41007876926a20f821d72d9c6f2c9dae6c03954123ea6e6939d7e6e669438891",
	  "2d98ea01f754d34bbc3003df5050200abf445ec728556d7ed7d5c54c55552b6d",
	  "9b52672742d637a32add056dfd6d8792f2a33c2e69dafabea09b960bc61e230a",
	  "06108e525f845d0155bf60193222b3219c98e3d49424c2fb2a0987f825c17959",
	  "62b5cdd591e5b507e560167ba8f6f7cda74673eb315680cb89ccbc4eec477dce", true, "P (0 )" },
};

// CAVP PKV [P-256]: a coordinate of 65 hex digits is at or above 2^256 and has no 32-byte encoding
static const struct { const char *qx, *qy; bool ok; const char *why; } pkv[] = {
	{ "e0f7449c5588f24492c338f2bc8f7865f755b958d48edb0f2d0056e50c3fd5b7",
	  "86d7e9255d0f4b6f44fa2cd6f8ba3c0aa828321d6d8cc430ca6284ce1d5b43a0", true, "P (0 )" },
	{ "d17c446237d9df87266ba3a91ff27f45abfdcb77bfd83536e92903efb861a9a9",
	  "1eabb6a349ce2cd447d777b6739c5fc066add2002d2029052c408d0701066231c", false, "F (1 - Q_x or Q_y out of range)" },
	{ "17875397ae87369365656d490e8ce956911bd97607f2aff41b56f6f3a61989826",
	  "980a3c4f61b9692633fbba5ef04c9cb546dd05cdec9fa8428b8849670e2fba92", false, "F (1 - Q_x or Q_y out of range)" },
	{ "f2d1c0dc0852c3d8a2a2500a23a44813ccce1ac4e58444175b440469ffc12273",
	  "32bfe992831b305d8c37b9672df5d29fcb5c29b4a40534683e3ace23d24647dd", false, "F (2 - Point not on curve)" },
	{ "10b0ca230fff7c04768f4b3d5c75fa9f6c539bea644dffbec5dc796a213061b58",
	  "f5edf37c11052b75f771b7f9fa050e353e464221fec916684ed45b6fead38205", false, "F (1 - Q_x or Q_y out of range)" },
	{ "2c1052f25360a15062d204a056274e93cbe8fc4c4e9b9561134ad5c15ce525da",
	  "ced9783713a8a2a09eff366987639c625753295d9a85d0f5325e32dedbcada0b", true, "P (0 )" },
	{ "a40d077a87dae157d93dcccf3fe3aca9c6479a75aa2669509d2ef05c7de6782f",
	  "503d86b87d743ba20804fd7e7884aa017414a7b5b5963e0d46e3a9611419ddf3", false, "F (2 - Point not on curve)" },
	{ "2633d398a3807b1895548adbb0ea2495ef4b930f91054891030817df87d4ac0a",
	  "d6b2f738e3873cc8364a2d364038ce7d0798bb092e3dd77cbdae7c263ba618d2", true, "P (0 )" },
	{ "14bf57f76c260b51ec6bbc72dbd49f02a56eaed070b774dc4bad75a54653c3d56",
	  "7a231a23bf8b3aa31d9600d888a0678677a30e573decd3dc56b33f365cc11236", false, "F (1 - Q_x or Q_y out of range)" },
	{ "2fa74931ae816b426f484180e517f5050c92decfc8daf756cd91f54d51b302f1",
	  "5b994346137988c58c14ae2152ac2f6ad96d97decb33099bd8a0210114cd1141", true, "P (0 )" },
	{ "f8c6dd3181a76aa0e36c2790bba47041acbe7b1e473ff71eee39a824dc595ff0",
	  "9c965f227f281b3072b95b8daf29e88b35284f3574462e268e529bbdc50e9e52", false, "F (2 - Point not on curve)" },
	{ "7a81a7e0b015252928d8b36e4ca37e92fdc328eb25c774b4f872693028c4be38",
	  "08862f7335147261e7b1c3d055f9a316e4cab7daf99cc09d1c647f5dd6e7d5bb", false, "F (2 - Point not on curve)" },
};

// CAVP KeyPair [P-256]: Q = d G
static const struct { const char *d, *qx, *qy; } keypair[] = {
	{ "c9806898a0334916c860748880a541f093b579a9b1f32934d86c363c39800357",
	  "d0720dc691aa80096ba32fed1cb97c2b620690d06de0317b8618d5ce65eb728f",
	  "9681b517b1cda17d0d83d335d9c4a8a9a9b0b1b3c7106d8f3c72bc5093dc275f" },
	{ "710735c8388f48c684a97bd66751cc5f5a122d6b9a96a2dbe73662f78217446d",
	  "f6836a8add91cb182d8d258dda6680690eb724a66dc3bb60d2322565c39e4ab9",
	  "1f837aa32864870cb8e8d0ac2ff31f824e7beddc4bb7ad72c173ad974b289dc2" },
	{ "78d5d8b7b3e2c16b3e37e7e63becd8ceff61e2ce618757f514620ada8a11f6e4",
	  "76711126cbb2af4f6a5fe5665dad4c88d27b6cb018879e03e54f779f203a854e",
	  "a26df39960ab5248fd3620fd018398e788bd89a3cea509b352452b69811e6856" },
	{ "2a61a0703860585fe17420c244e1de5a6ac8c25146b208ef88ad51ae34c8cb8c",
	  "e1aa7196ceeac088aaddeeba037abb18f67e1b55c0a5c4e71ec70ad666fcddc8",
	  "d7d35bdce6dedc5de98a7ecb27a9cd066a08f586a733b59f5a2cdb54f971d5c8" },
	{ "01b965b45ff386f28c121c077f1d7b2710acc6b0cb58d8662d549391dcf5a883",
	  "1f038c5422e88eec9e88b815e8f6b3e50852333fc423134348fc7d79ef8e8a10",
	  "43a047cb20e94b4ffb361ef68952b004c0700b2962e0c0635a70269bc789b849" },
	{ "fac92c13d374c53a085376fe4101618e1e181b5a63816a84a0648f3bdc24e519",
	  "7258f2ab96fc84ef6ccb33e308cd392d8b568ea635730ceb4ebd72fa870583b9",
	  "489807ca55bdc29ca5c8fe69b94f227b0345cccdbe89975e75d385cc2f6bb1e2" },
	{ "f257a192dde44227b3568008ff73bcf599a5c45b32ab523b5b21ca582fef5a0a",
	  "d2e01411817b5512b79bbbe14d606040a4c90deb09e827d25b9f2fc068997872",
	  "503f138f8bab1df2c4507ff663a1fdf7f710e7adb8e7841eaa902703e314e793" },
	{ "add67e57c42a3d28708f0235eb86885a4ea68e0d8cfd76eb46134c596522abfd",
	  "55bed2d9c029b7f230bde934c7124ed52b1330856f13cbac65a746f9175f85d7",
	  "32805e311d583b4e007c40668185e85323948e21912b6b0d2cda8557389ae7b0" },
	{ "4494860fd2c805c5c0d277e58f802cff6d731f76314eb1554142a637a9bc5538",
	  "5190277a0c14d8a3d289292f8a544ce6ea9183200e51aec08440e0c1a463a4e4",
	  "ecd98514821bd5aaf3419ab79b71780569470e4fed3da3c1353b28fe137f36eb" },
	{ "d40b07b1ea7b86d4709ef9dc634c61229feb71abd63dc7fc85ef46711a87b210",
	  "fbcea7c2827e0e8085d7707b23a3728823ea6f4878b24747fb4fd2842d406c73",
	  "2393c85f1f710c5afc115a39ba7e18abe03f19c9d4bb3d47d19468b818efa535" },
};

// P256-EDGE (derived, checked with OpenSSL: p256.md 12): with d, k is RFC 6979's and r, s the signature; without, a
// verification
static const struct { const char *d, *k, *qx, *qy, *h, *r, *s; bool ok; } edge[] = {
	{ "3f9bc000fab83aed4551ec40a5f6a7c178b255ba654c0684d242bc8f59f8da18", "5031bc6fee35c5b03ee483d81d9dda44d0bef35c364db548363d57dce5cdc13f",
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
	  "cb3c16841c93631a0c4b05faef105f8b44fe3cbed9fa93e9e2fd5916e457e501",
	  "ecb9c189d18e1e19215da18e9abad0b212b68310ed1ccffe3eee2ed92aa8f346", true },
	{ "0000000000000000000000000000000000000000000000000000000000000001", "ca6d99035bcb7c447badb1ae65f69638a69605c167deb689a61319330428be7b",
	  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
	  "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "5cec92294640aab1721c8ffe5546cb8faba290021a5e369f00d0a4b39c9cfd9c",
	  "677ada92e8c5d30c58223baf4684c96b128f3b31250b4dacd449d13275763fe2", true },
	{ "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550", "10c14eef219f45efbfba1abd528cd2e9f154a1a884ea726aa4c7973f93fe942e",
	  "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
	  "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "84c52489d26189d9a681f1571c00d71c7af568a2095f2cae03b5dd8604e0aeb7",
	  "1c2d6526522100b04a9da8a5c3b1d49f6cd7c1ea081c5945ea6ad82544aad82f", true },
	{ NULL, NULL,
	  "049a0dd8b23422e24cd2259b3525720ac35ace28bd32c5e0b2bff95c9f55e54d",
	  "6dc51acac9f5218f114860f0fdb925ab500a82366941285a670c0106332efa6c",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "0000000000000000000000000000000000000000000000000000000000000003",
	  "0000000000000000000000000000000000000000000000000000000001234567", true },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "0000000000000000000000000000000000000000000000000000000000000000",
	  "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54", false },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "e689788a5c2fbdd0fa2137429517b7c9dd5e3c2bffb9667d4edb9a8472f3cf31",
	  "0000000000000000000000000000000000000000000000000000000000000000", false },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
	  "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54", false },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "e689788a5c2fbdd0fa2137429517b7c9dd5e3c2bffb9667d4edb9a8472f3cf31",
	  "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", false },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
	  "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54", false },
	{ NULL, NULL,
	  "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3",
	  "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea071",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "e689788a5c2fbdd0fa2137429517b7c9dd5e3c2bffb9667d4edb9a8472f3cf31",
	  "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54", false },
	{ NULL, NULL,
	  "0000000000000000000000000000000000000000000000000000000000000000",
	  "0000000000000000000000000000000000000000000000000000000000000000",
	  "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d",
	  "e689788a5c2fbdd0fa2137429517b7c9dd5e3c2bffb9667d4edb9a8472f3cf31",
	  "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54", false },
};

// M17-SIG (M17 v2.1.1's test vector; k from RFC 6979 with h1 = H)
static const char *m17_hashinput = "4d31372d5349472d76320000000102030405060708090a0b0c0d0e0f0001101112131415161718191a1b1c1d1e1f0002202122232425262728292a2b2c2d2e2f7ffb00004b13d1060000001680b70cf10980";
static const char *m17_h = "2dd1bc7b5c017f100f9379f42eaa205ce90b8c7ea263f2f80e37ba6e6d6b588d";
static const char *m17_d = "3f9bc000fab83aed4551ec40a5f6a7c178b255ba654c0684d242bc8f59f8da18";
static const char *m17_qx = "b91931f4dc5efc54c23c0b8c87704c45585bd791046fef05bfbfd5a0ce9b53b3";
static const char *m17_qy = "c55637387871bb5491a814c2344ec6f0696bf3e16fe8017fc08e3a6320eea070";
static const char *m17_k = "49a22af2b7dfb250f6af5f35f72441fd2dfaee029f324e6d672bcdd8dfc5b51f";
static const char *m17_r = "e689788a5c2fbdd0fa2137429517b7c9dd5e3c2bffb9667d4edb9a8472f3cf31";
static const char *m17_s = "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d54";
static const char *m17_s_bad = "c45421ded7f7025608bfd928b06befd4a44526609b3dc54fc2b36944f6121d55";
static const char *m17_s_twin = "3babde202808fdaaf74026d74f94102b18a1d44d0bd9d9353106617e065107fd";
static const bool m17_s_bad_ok = false, m17_s_twin_ok = true;

// Computed here: a point with a small x; an x with no point
static const char *small_x = "0000000000000000000000000000000000000000000000000000000000000005",
	*small_y = "459243b9aa581806fe913bce99817ade11ca503c64d9a3c533415c083248fbcc",
	*small_xp = "ffffffff00000001000000000000000000000001000000000000000000000004";
static const char *no_x = "0000000000000000000000000000000000000000000000000000000000000001";

// ---- The field, the constants, G's table ----

static uint32_t *scratch_w(void)
{
	return ctx_of(S)->w;
}

// A plain value (hex) to Montgomery words
static void to_m(uint32_t r[8], const char *h)
{
	uint8_t b[32];

	hex(h, b, 32);
	be_load(r, b);
	fe_mul(scratch_w(), r, r, RR_P);
}

static void test_field(void)
{
	ctx_t *c = ctx_of(S);
	uint32_t x[8], y[8], z[8];
	uint8_t b[32];

	// P256-DOMAIN: p and n as the code has them; b and G are checked below and by the table
	be_store(b, P);
	check(eq_hex(b, 32, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff"), "P256-DOMAIN: p");
	be_store(b, N);
	check(eq_hex(b, 32, "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"), "P256-DOMAIN: n");
	check(memcmp(b, N_BE, 32) == 0, "P256-DOMAIN: n as bytes");
	memset(z, 0, 32);
	sc_mul(c->w, z, N, ONE);	// n * 2^-256 mod n = 0, and N0 makes the rounds exact
	check(is_zero(z) && (uint32_t)(N[0] * N0) == 0xffffffffu, "-n^-1 mod 2^32");
	// P256-FIELD: x y 2^-256 on the plain values, and x y through Montgomery form
	hex(field_x, b, 32);
	be_load(x, b);
	hex(field_y, b, 32);
	be_load(y, b);
	fe_mul(c->w, z, x, y);
	be_store(b, z);
	check(eq_hex(b, 32, field_mont), "P256-FIELD: x y 2^-256 mod p");
	to_m(x, field_x);
	to_m(y, field_y);
	fe_mul(c->w, z, x, y);
	fe_mul(c->w, z, z, ONE);
	be_store(b, z);
	check(eq_hex(b, 32, field_xy), "P256-FIELD: x y mod p");
	// inverse: a a^-1 = 1, for a = x and a = 1
	fe_inv(c, z, x);
	fe_mul(c->w, z, z, x);
	check(memcmp(z, ONE_M, 32) == 0, "field inverse of x");
	fe_inv(c, z, ONE_M);
	check(memcmp(z, ONE_M, 32) == 0, "field inverse of 1");
	// p - 1 squared is 1
	memcpy(x, P, 32);
	x[0]--;
	fe_mul(c->w, x, x, RR_P);
	fe_mul(c->w, x, x, x);
	check(memcmp(x, ONE_M, 32) == 0, "(p - 1)^2 = 1");
	// add and sub wrap: (p - 1) + (p - 1) = p - 2, 0 - 1 = p - 1, 0 negated is 0
	memcpy(x, P, 32);
	x[0]--;
	fe_add(y, x, x);
	memcpy(z, P, 32);
	z[0] -= 2;
	check(memcmp(y, z, 32) == 0, "field add past 2^256");
	fe_sub(y, ZERO, ONE);
	check(memcmp(y, x, 32) == 0, "field sub below 0");
	fe_neg(y, ZERO);
	check(is_zero(y), "-0 = 0");
	// scalars: a a^-1 = 1 mod n for a = n - 1 and a = 2
	memcpy(x, N, 32);
	x[0]--;
	sc_mul(c->w, x, x, RR_N);
	sc_inv(c->w, y, x, c->u.vf.x.pow);
	sc_mul(c->w, y, y, x);
	sc_mul(c->w, y, y, ONE);
	check(memcmp(y, ONE, 32) == 0, "scalar inverse of n - 1");
	{
		// 2^-1 + 2^-1 = 1
		uint32_t two[8] = { 2u }, inv[8];
		sc_mul(c->w, inv, two, RR_N);
		sc_inv(c->w, inv, inv, c->u.vf.x.pow);
		sc_mul(c->w, inv, inv, ONE);		// plain 2^-1 mod n
		mod_add(inv, inv, inv, N);
		check(memcmp(inv, ONE, 32) == 0, "scalar inverse of 2");
	}
	// b and G as stored
	fe_mul(c->w, z, B_M, ONE);
	be_store(b, z);
	check(eq_hex(b, 32, "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"), "b in Montgomery form");
	fe_mul(c->w, z, THREE_M, ONE);
	check(z[0] == 3 && all_zero(z + 1, 28), "3 in Montgomery form");
	fe_mul(c->w, z, ONE_M, ONE);
	check(memcmp(z, ONE, 32) == 0, "1 in Montgomery form");
}

// Every entry of p256_tab.c from G alone: 2G by doubling, then (2i + 1) G = (2i - 1) G + 2G, through the Jacobian
// formulas and the affine conversion, compared in Montgomery form
static void test_table(void)
{
	ctx_t *c = ctx_of(S);
	jac_t g, g2, acc;
	uint32_t x[8], y[8];
	char what[64];

	to_m(g.x, "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
	to_m(g.y, "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
	memcpy(g.z, ONE_M, 32);
	pt_dbl(c, &g2, &g);
	acc = g;
	for (unsigned i = 0; i < 8; i++) {
		if (i)
			(void)pt_add_vt(c, &acc, &acc, &g2, false);
		pt_affine(c, x, y, &acc);
		fe_mul(c->w, x, x, RR_P);
		fe_mul(c->w, y, y, RR_P);
		snprintf(what, sizeof what, "table entry %uG", 2 * i + 1);
		check(memcmp(x, p256_tab_g[i], 32) == 0 && memcmp(y, p256_tab_g[i] + 8, 32) == 0, what);
	}
	// the mixed addition agrees with the Jacobian one: 2G + G = 3G, its doubling case and its identity case
	{
		aff_t a;
		uint32_t same;
		memcpy(a.x, p256_tab_g[0], 32);
		memcpy(a.y, p256_tab_g[0] + 8, 32);
		acc = g2;
		same = pt_madd(c, &acc, &acc, &a);
		pt_affine(c, x, y, &acc);
		fe_mul(c->w, x, x, RR_P);
		check(!same && memcmp(x, p256_tab_g[1], 32) == 0, "mixed addition 2G + G");
		acc = g;
		check(pt_madd(c, &acc, &acc, &a) != 0, "mixed addition G + G reports the doubling");
		acc = g;
		fe_neg(a.y, a.y);
		check(pt_madd(c, &acc, &acc, &a) == 0 && is_zero(acc.z), "mixed addition G - G is the identity");
	}
}

// ---- Keys ----

static void test_mult(void)
{
	uint8_t d[32], q[64];
	char what[96];

	for (size_t v = 0; v < sizeof mult / sizeof mult[0]; v++) {
		hex(mult[v].k, d, 32);
		snprintf(what, sizeof what, "P256-MULT k = %.16s...", mult[v].k);
		check(p256_pub_from_priv(d, q, v & 1 ? S4 : S) && eq_hex(q, 32, mult[v].qx) && eq_hex(q + 32, 32, mult[v].qy),
		      what);
		check(p256_pub_valid(q), "P256-MULT Q valid");
	}
	for (size_t v = 0; v < sizeof keypair / sizeof keypair[0]; v++) {
		hex(keypair[v].d, d, 32);
		snprintf(what, sizeof what, "CAVP KeyPair %zu", v);
		check(p256_pub_from_priv(d, q, S) && eq_hex(q, 32, keypair[v].qx) && eq_hex(q + 32, 32, keypair[v].qy), what);
	}
}

static void test_pkv(void)
{
	uint8_t q[64];
	char what[96];

	for (size_t v = 0; v < sizeof pkv / sizeof pkv[0]; v++) {
		snprintf(what, sizeof what, "CAVP PKV %zu: %s", v, pkv[v].why);
		if (strlen(pkv[v].qx) > 64 || strlen(pkv[v].qy) > 64) {
			// at or above 2^256: no 64-byte form exists, so it can't reach the radio
			check(!pkv[v].ok, what);
			continue;
		}
		hex2(pkv[v].qx, pkv[v].qy, q);
		check(p256_pub_valid(q) == pkv[v].ok, what);
	}
}

// Compression round trips for every valid key in the vectors, and what decompression refuses
static void test_compress(void)
{
	uint8_t q[64], c[33], back[64];
	unsigned n = 0, ok = 0;
	const char *keys[][2] = {
		{ rfc_qx, rfc_qy }, { m17_qx, m17_qy }, { small_x, small_y },
	};

	for (size_t v = 0; v < sizeof mult / sizeof mult[0]; v++, n++) {
		hex2(mult[v].qx, mult[v].qy, q);
		p256_pub_compress(q, c);
		ok += p256_pub_decompress(c, back) && memcmp(q, back, 64) == 0 && c[0] == (2 | (q[63] & 1));
	}
	for (size_t v = 0; v < sizeof keypair / sizeof keypair[0]; v++, n++) {
		hex2(keypair[v].qx, keypair[v].qy, q);
		p256_pub_compress(q, c);
		ok += p256_pub_decompress(c, back) && memcmp(q, back, 64) == 0;
	}
	for (size_t v = 0; v < sizeof siggen / sizeof siggen[0]; v++, n++) {
		hex2(siggen[v].qx, siggen[v].qy, q);
		p256_pub_compress(q, c);
		ok += p256_pub_decompress(c, back) && memcmp(q, back, 64) == 0;
	}
	for (size_t v = 0; v < sizeof keys / sizeof keys[0]; v++, n++) {
		hex2(keys[v][0], keys[v][1], q);
		p256_pub_compress(q, c);
		ok += p256_pub_decompress(c, back) && memcmp(q, back, 64) == 0;
	}
	check(ok == n, "compression round trips");
	printf("p256Test: %u compression round trips\n", n);
	// the other y: prefix flipped gives p - y
	hex2(m17_qx, m17_qy, q);
	p256_pub_compress(q, c);
	c[0] ^= 1;
	check(p256_pub_decompress(c, back) && p256_pub_valid(back) && memcmp(back + 32, q + 32, 32) != 0,
	      "decompress the other y");
	// refused: a bad prefix, an x with no point, x = p, x above p
	hex2(m17_qx, m17_qy, q);
	p256_pub_compress(q, c);
	c[0] = 4;
	check(!p256_pub_decompress(c, back), "decompress refuses prefix 04");
	c[0] = 0;
	check(!p256_pub_decompress(c, back), "decompress refuses prefix 00");
	c[0] = 2;
	hex(no_x, c + 1, 32);
	check(!p256_pub_decompress(c, back), "decompress refuses an x off the curve");
	hex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", c + 1, 32);
	check(!p256_pub_decompress(c, back), "decompress refuses x = p");
	hex(small_xp, c + 1, 32);
	check(!p256_pub_decompress(c, back), "decompress refuses x + p for an x on the curve");
}

static void test_invalid(void)
{
	uint8_t d[32], q[64], rs[64], k[32], h[32];
	static const char *bad[] = {
		"0000000000000000000000000000000000000000000000000000000000000000",
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",	// n
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632552",
		"ffffffff00000001000000000000000000000000000000000000000000000000",
		"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
	};
	char what[96];

	hex(m17_h, h, 32);
	hex(m17_k, k, 32);
	for (size_t v = 0; v < sizeof bad / sizeof bad[0]; v++) {
		hex(bad[v], d, 32);
		memset(rs, 0x5a, sizeof rs);
		snprintf(what, sizeof what, "invalid d %.12s... refused", bad[v]);
		check(!p256_priv_valid(d) && !p256_pub_from_priv(d, q, S) && !p256_sign(d, h, rs, S) &&
		      !p256_sign_k(d, h, k, rs, S) && !p256_rfc6979_k(d, h, k, S) && all_zero(rs, 64), what);
		// as a nonce
		hex(m17_d, q, 32);
		snprintf(what, sizeof what, "invalid k %.12s... refused", bad[v]);
		check(!p256_sign_k(q, h, d, rs, S), what);
	}
	hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550", d, 32);
	check(p256_priv_valid(d), "d = n - 1 valid");
	hex("0000000000000000000000000000000000000000000000000000000000000001", d, 32);
	check(p256_priv_valid(d), "d = 1 valid");

	// points: off the curve, zero, x + p for a point on the curve, a coordinate equal to p, G's y negated is fine
	hex2(m17_qx, m17_qy, q);
	check(p256_pub_valid(q), "M17 key valid");
	q[63] ^= 1;
	check(!p256_pub_valid(q), "M17 key, y changed, refused");
	memset(q, 0, 64);
	check(!p256_pub_valid(q), "all-zero key refused");
	hex2(small_x, small_y, q);
	check(p256_pub_valid(q), "small-x point valid");
	hex2(small_xp, small_y, q);
	check(!p256_pub_valid(q), "x + p refused (range before the curve)");
	hex2("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", small_y, q);
	check(!p256_pub_valid(q), "x = p refused");
	hex2(small_x, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", q);
	check(!p256_pub_valid(q), "y = p refused");
	// verifying with an invalid key refuses even a signature that would check against the right one
	hex2(m17_qx, m17_qy, q);
	q[0] ^= 0x80;
	hex(m17_r, rs, 32);
	hex(m17_s, rs + 32, 32);
	check(!p256_verify(q, h, rs, S), "verify refuses an off-curve key");
}

// ---- Signatures ----

static void test_rfc6979(void)
{
	uint8_t d[32], h[32], k[32], rs[64], q[64];
	char what[96];

	hex(rfc_d, d, 32);
	check(p256_pub_from_priv(d, q, S) && eq_hex(q, 32, rfc_qx) && eq_hex(q + 32, 32, rfc_qy), "RFC 6979 A.2.5 key");
	for (size_t v = 0; v < sizeof rfc / sizeof rfc[0]; v++) {
		uint8_t hh[32];
		sha(rfc[v].msg, strlen(rfc[v].msg), hh);
		hex(rfc[v].h, h, 32);
		snprintf(what, sizeof what, "RFC 6979 %s: H", rfc[v].msg);
		check(memcmp(h, hh, 32) == 0, what);
		snprintf(what, sizeof what, "RFC 6979 %s: k", rfc[v].msg);
		check(p256_rfc6979_k(d, h, k, S) && eq_hex(k, 32, rfc[v].k), what);
		snprintf(what, sizeof what, "RFC 6979 %s: signature", rfc[v].msg);
		check(p256_sign(d, h, rs, S4) && eq_hex(rs, 32, rfc[v].r) && eq_hex(rs + 32, 32, rfc[v].s), what);
		snprintf(what, sizeof what, "RFC 6979 %s: verify", rfc[v].msg);
		check(p256_verify(q, h, rs, S), what);
	}
}

static void test_siggen(void)
{
	uint8_t d[32], h[32], k[32], rs[64], q[64];
	char what[96];

	for (size_t v = 0; v < sizeof siggen / sizeof siggen[0]; v++) {
		hex(siggen[v].d, d, 32);
		hex(siggen[v].h, h, 32);
		hex(siggen[v].k, k, 32);
		snprintf(what, sizeof what, "CAVP SigGen %zu: Q", v);
		check(p256_pub_from_priv(d, q, S) && eq_hex(q, 32, siggen[v].qx) && eq_hex(q + 32, 32, siggen[v].qy), what);
		snprintf(what, sizeof what, "CAVP SigGen %zu: r, s with the given k", v);
		check(p256_sign_k(d, h, k, rs, S) && eq_hex(rs, 32, siggen[v].r) && eq_hex(rs + 32, 32, siggen[v].s), what);
		snprintf(what, sizeof what, "CAVP SigGen %zu: verifies", v);
		check(p256_verify(q, h, rs, S), what);
		// and RFC 6979's own signature of the same hash verifies too
		snprintf(what, sizeof what, "CAVP SigGen %zu: deterministic signature verifies", v);
		check(p256_sign(d, h, rs, S) && p256_verify(q, h, rs, S), what);
	}
}

static void test_sigver(void)
{
	uint8_t h[32], q[64], rs[64];
	char what[96];

	for (size_t v = 0; v < sizeof sigver / sizeof sigver[0]; v++) {
		hex(sigver[v].h, h, 32);
		hex2(sigver[v].qx, sigver[v].qy, q);
		hex(sigver[v].r, rs, 32);
		hex(sigver[v].s, rs + 32, 32);
		snprintf(what, sizeof what, "CAVP SigVer %zu: %s", v, sigver[v].why);
		check(p256_verify(q, h, rs, v & 1 ? S4 : S) == sigver[v].ok, what);
	}
}

static void test_edge(void)
{
	uint8_t d[32], h[32], k[32], q[64], rs[64], got[64];
	char what[96];

	for (size_t v = 0; v < sizeof edge / sizeof edge[0]; v++) {
		hex(edge[v].h, h, 32);
		hex2(edge[v].qx, edge[v].qy, q);
		hex(edge[v].r, rs, 32);
		hex(edge[v].s, rs + 32, 32);
		if (edge[v].d) {
			hex(edge[v].d, d, 32);
			snprintf(what, sizeof what, "P256-EDGE %zu: k", v);
			check(p256_rfc6979_k(d, h, k, S) && eq_hex(k, 32, edge[v].k), what);
			snprintf(what, sizeof what, "P256-EDGE %zu: signature", v);
			check(p256_sign(d, h, got, S) && memcmp(got, rs, 64) == 0, what);
			snprintf(what, sizeof what, "P256-EDGE %zu: Q", v);
			check(p256_pub_from_priv(d, got, S) && memcmp(got, q, 64) == 0, what);
		}
		snprintf(what, sizeof what, "P256-EDGE %zu: verify %s", v, edge[v].ok ? "accepts" : "refuses");
		check(p256_verify(q, h, rs, S) == edge[v].ok, what);
	}
}

static void test_m17(void)
{
	uint8_t in[128], h[32], d[32], k[32], q[64], rs[64];
	size_t n = hex(m17_hashinput, in, 0);

	hex(m17_hashinput, in, n);
	sha(in, n, h);
	check(eq_hex(h, 32, m17_h), "M17-SIG: H");
	sha("M17 test key", 12, d);
	check(eq_hex(d, 32, m17_d), "M17-SIG: d = SHA-256(\"M17 test key\")");
	check(p256_pub_from_priv(d, q, S) && eq_hex(q, 32, m17_qx) && eq_hex(q + 32, 32, m17_qy), "M17-SIG: Q");
	check(p256_rfc6979_k(d, h, k, S) && eq_hex(k, 32, m17_k), "M17-SIG: RFC 6979 k");
	check(p256_sign(d, h, rs, S) && eq_hex(rs, 32, m17_r) && eq_hex(rs + 32, 32, m17_s), "M17-SIG: r, s");
	check(p256_sign_k(d, h, k, rs, S) && eq_hex(rs, 32, m17_r) && eq_hex(rs + 32, 32, m17_s), "M17-SIG: with k");
	check(rs[32] & 0x80, "M17-SIG: s is high (above n / 2)");
	check(p256_verify(q, h, rs, S), "M17-SIG: the high-s signature verifies");
	hex(m17_s_bad, rs + 32, 32);
	check(p256_verify(q, h, rs, S) == m17_s_bad_ok && !m17_s_bad_ok, "M17-SIG: s + 1 refused");
	hex(m17_s_twin, rs + 32, 32);
	check(p256_verify(q, h, rs, S) == m17_s_twin_ok && m17_s_twin_ok, "M17-SIG: n - s verifies");
	// a changed hash, r, key
	hex(m17_s, rs + 32, 32);
	h[31] ^= 1;
	check(!p256_verify(q, h, rs, S), "M17-SIG: changed H refused");
	h[31] ^= 1;
	rs[0] ^= 1;
	check(!p256_verify(q, h, rs, S), "M17-SIG: changed r refused");
	rs[0] ^= 1;
	hex2(rfc_qx, rfc_qy, q);
	check(!p256_verify(q, h, rs, S), "M17-SIG: another key refused");
}

// A small PRNG (xorshift64*) for the round trips: fixed seed, the run is the same every time
static uint64_t rng = 0x9e3779b97f4a7c15u;
static void rnd(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		p[i] = (uint8_t)((rng * 0x2545f4914f6cdd1du) >> 56);
	}
}

static void test_round_trips(void)
{
	uint8_t d[32], h[32], q[64], rs[64];
	unsigned ok = 0, n = 24;

	for (unsigned i = 0; i < n; i++) {
		rnd(d, 32);
		rnd(h, 32);
		if (i == 0)
			memset(h, 0, 32);			// e = 0: u = 0
		if (i == 1)
			memcpy(h, N_BE, 32);		// e = n: also 0 mod n
		if (i == 2)
			d[0] = 0;			// a short key
		if (!p256_priv_valid(d))
			continue;
		bool good = p256_pub_from_priv(d, q, S) && p256_sign(d, h, rs, S) && p256_verify(q, h, rs, S);
		h[i % 32] ^= (uint8_t)(1u << (i % 8));
		good = good && !p256_verify(q, h, rs, S);
		ok += good;
	}
	check(ok == n, "random sign / verify round trips");
}

// ---- Secrets ----

static void test_wipe(void)
{
	uint8_t d[32], h[32], k[32], q[64], rs[64];

	hex(m17_d, d, 32);
	hex(m17_h, h, 32);
	hex(m17_k, k, 32);
	memset(&scr, 0xa5, sizeof scr);
	check(p256_sign(d, h, rs, S) && all_zero(S, sizeof *S), "scratch zero after sign");
	memset(&scr, 0xa5, sizeof scr);
	check(p256_sign(d, h, rs, S4) && all_zero(S4, sizeof *S4), "scratch zero after sign, 4-aligned");
	memset(&scr, 0xa5, sizeof scr);
	check(p256_sign_k(d, h, k, rs, S) && all_zero(S, sizeof *S), "scratch zero after sign_k");
	memset(&scr, 0xa5, sizeof scr);
	check(p256_pub_from_priv(d, q, S) && all_zero(S, sizeof *S), "scratch zero after pub_from_priv");
	memset(&scr, 0xa5, sizeof scr);
	check(p256_rfc6979_k(d, h, k, S) && all_zero(S, sizeof *S), "scratch zero after rfc6979_k");
	memset(d, 0, 32);
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_sign(d, h, rs, S) && all_zero(S, sizeof *S), "scratch zero after a refused sign");
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_sign_k(d, h, k, rs, S) && all_zero(S, sizeof *S), "scratch zero after a refused sign_k (d)");
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_pub_from_priv(d, q, S) && all_zero(S, sizeof *S), "scratch zero after a refused pub_from_priv");
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_rfc6979_k(d, h, k, S) && all_zero(S, sizeof *S), "scratch zero after a refused rfc6979_k");
	// a valid d with k = 0 or k = n: refused, the scratch zero all the same
	hex(m17_d, d, 32);
	memset(k, 0, 32);
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_sign_k(d, h, k, rs, S) && all_zero(S, sizeof *S) && all_zero(rs, 64),
	      "scratch and signature zero after k = 0");
	memcpy(k, N_BE, 32);
	memset(&scr, 0xa5, sizeof scr);
	check(!p256_sign_k(d, h, k, rs, S) && all_zero(S, sizeof *S), "scratch zero after k = n");
	memset(&scr, 0xa5, sizeof scr);
	p256_wipe(S);
	check(all_zero(S, sizeof *S), "p256_wipe");
	p256_wipe(NULL);
	// the context with its alignment fits the scratch
	check(sizeof(ctx_t) + 8 <= P256_SCRATCH_BYTES && (uintptr_t)ctx_of(S4) % 8 == 0 &&
	      (uint8_t *)ctx_of(S4) + sizeof(ctx_t) <= (uint8_t *)S4 + sizeof *S4, "context layout inside the scratch");
	printf("p256Test: context %zu of %u scratch bytes (sign %zu, verify %zu)\n", sizeof(ctx_t),
	       (unsigned)P256_SCRATCH_BYTES, offsetof(ctx_t, u) + sizeof(sign_t), offsetof(ctx_t, u) + sizeof(verify_t));
}

// The same field multiplies for every key: d of weight 1, 128 and 255, even and odd (n - k), short and long. Signing
// and Q = d G; the RFC 6979 loop only hashes
static void test_constant_count(void)
{
	static const char *keys[] = {
		"0000000000000000000000000000000000000000000000000000000000000001",
		"0000000000000000000000000000000000000000000000000000000000000002",
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550",
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254f",
		"5555555555555555555555555555555555555555555555555555555555555555",
		"8000000000000000000000000000000000000000000000000000000000000000",
		"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		"3f9bc000fab83aed4551ec40a5f6a7c178b255ba654c0684d242bc8f59f8da18",
	};
	uint8_t d[32], h[32], rs[64], q[64];
	unsigned long sign_n = 0, pub_n = 0, signk_n = 0;
	bool same = true;

	hex(m17_h, h, 32);
	for (size_t v = 0; v < sizeof keys / sizeof keys[0]; v++) {
		hex(keys[v], d, 32);
		p256_test_muls = 0;
		same &= p256_sign(d, h, rs, S);
		if (v && p256_test_muls != sign_n)
			same = false;
		sign_n = p256_test_muls;
		p256_test_muls = 0;
		same &= p256_pub_from_priv(d, q, S);
		if (v && p256_test_muls != pub_n)
			same = false;
		pub_n = p256_test_muls;
		// the key as the nonce: k of the same weights
		p256_test_muls = 0;
		same &= p256_sign_k(d, h, d, rs, S);
		if (v && p256_test_muls != signk_n)
			same = false;
		signk_n = p256_test_muls;
	}
	check(same, "the same multiplies for every key and nonce");
	printf("p256Test: multiplies per sign %lu, per Q = d G %lu\n", sign_n, pub_n);
	hex2(m17_qx, m17_qy, q);
	hex(m17_r, rs, 32);
	hex(m17_s, rs + 32, 32);
	p256_test_muls = 0;
	check(p256_verify(q, h, rs, S), "verify for the count");
	printf("p256Test: multiplies per verify (M17-SIG) %lu\n", p256_test_muls);
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void bench(void)
{
	uint8_t d[32], h[32], q[64], rs[64];
	unsigned n = 200;
	double t0;
	bool ok = true;

	hex(m17_d, d, 32);
	hex(m17_h, h, 32);
	hex2(m17_qx, m17_qy, q);
	t0 = now_s();
	for (unsigned i = 0; i < n; i++)
		ok &= p256_sign(d, h, rs, S);
	printf("p256Test bench: sign %.3f ms\n", (now_s() - t0) * 1e3 / n);
	t0 = now_s();
	for (unsigned i = 0; i < n; i++)
		ok &= p256_verify(q, h, rs, S);
	printf("p256Test bench: verify %.3f ms\n", (now_s() - t0) * 1e3 / n);
	t0 = now_s();
	for (unsigned i = 0; i < n * 200; i++)
		fe_mul(ctx_of(S)->w, ctx_of(S)->t[0], ctx_of(S)->t[0], RR_P);
	printf("p256Test bench: field multiply %.1f ns\n", (now_s() - t0) * 1e9 / (n * 200));
	if (!ok)
		printf("FAIL bench signatures\n");
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "bench") == 0) {
		bench();
		return 0;
	}
	printf("p256Test: %s multiply\n", P256_HALVES ? "16-bit halves" : "native 32 x 32 -> 64");
	test_field();
	test_table();
	test_mult();
	test_pkv();
	test_compress();
	test_invalid();
	test_rfc6979();
	test_siggen();
	test_sigver();
	test_edge();
	test_m17();
	test_round_trips();
	test_wipe();
	test_constant_count();
	if (failed) {
		printf("p256Test: %u of %u checks failed\n", failed, checks);
		return 1;
	}
	printf("p256Test: %u checks passed\n", checks);
	return 0;
}
