/**
 * @generated by contrib/devtools/chainparams/generate_chainparams_constants.py
 */

#include <chainparamsconstants.h>

namespace ChainParamsConstants {
    const BlockHash MAINNET_DEFAULT_ASSUME_VALID = BlockHash::fromHex("000000000000000013dd92c3d39846d8363fefd54fb78eb2a1a68886bfa7726f");
    const uint256 MAINNET_MINIMUM_CHAIN_WORK = uint256S("00000000000000000000000000000000000000000154368ca7cfaf045e903cb3");
    const uint64_t MAINNET_ASSUMED_BLOCKCHAIN_SIZE = 210;
    const uint64_t MAINNET_ASSUMED_CHAINSTATE_SIZE = 3;

    const BlockHash TESTNET_DEFAULT_ASSUME_VALID = BlockHash::fromHex("00000000000197f27596384671b5b914359f9fdf41dcb1674303407dacd3a7c4");
    const uint256 TESTNET_MINIMUM_CHAIN_WORK = uint256S("00000000000000000000000000000000000000000000006e80d22134f747ba0d");
    const uint64_t TESTNET_ASSUMED_BLOCKCHAIN_SIZE = 55;
    const uint64_t TESTNET_ASSUMED_CHAINSTATE_SIZE = 2;
} // namespace ChainParamsConstants

