#include "TestCommon.h"

#include "libAutoUpdater/interfaces/IHashProvider.h"

void testSha256Provider() {
    auto provider = autoupdater::createDefaultHashProvider();
    auto hash = provider->sha256Bytes("abc");
    LAU_REQUIRE(hash);
    LAU_REQUIRE(hash.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

