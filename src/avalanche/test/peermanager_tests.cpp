// Copyright (c) 2020 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <avalanche/delegationbuilder.h>
#include <avalanche/peermanager.h>
#include <avalanche/proofbuilder.h>
#include <avalanche/proofcomparator.h>
#include <avalanche/test/util.h>
#include <script/standard.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace avalanche;

namespace avalanche {
namespace {
    struct TestPeerManager {
        static bool nodeBelongToPeer(const PeerManager &pm, NodeId nodeid,
                                     PeerId peerid) {
            return pm.forNode(nodeid, [&](const Node &node) {
                return node.peerid == peerid;
            });
        }

        static bool isNodePending(const PeerManager &pm, NodeId nodeid) {
            auto &pendingNodesView = pm.pendingNodes.get<by_nodeid>();
            return pendingNodesView.find(nodeid) != pendingNodesView.end();
        }

        static PeerId registerAndGetPeerId(PeerManager &pm,
                                           const ProofRef &proof) {
            pm.registerProof(proof);

            auto &pview = pm.peers.get<by_proofid>();
            auto it = pview.find(proof->getId());
            return it == pview.end() ? NO_PEER : it->peerid;
        }

        static std::vector<uint32_t> getOrderedScores(const PeerManager &pm) {
            std::vector<uint32_t> scores;

            auto &peerView = pm.peers.get<by_score>();
            for (const Peer &peer : peerView) {
                scores.push_back(peer.getScore());
            }

            return scores;
        }
    };
} // namespace
} // namespace avalanche

namespace {
struct NoCoolDownFixture : public TestingSetup {
    NoCoolDownFixture() {
        gArgs.ForceSetArg("-avalancheconflictingproofcooldown", "0");
    }
    ~NoCoolDownFixture() {
        gArgs.ClearForcedArg("-avalancheconflictingproofcooldown");
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(peermanager_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(select_peer_linear) {
    // No peers.
    BOOST_CHECK_EQUAL(selectPeerImpl({}, 0, 0), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl({}, 1, 3), NO_PEER);

    // One peer
    const std::vector<Slot> oneslot = {{100, 100, 23}};

    // Undershoot
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 0, 300), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 42, 300), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 99, 300), NO_PEER);

    // Nailed it
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 100, 300), 23);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 142, 300), 23);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 199, 300), 23);

    // Overshoot
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 200, 300), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 242, 300), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(oneslot, 299, 300), NO_PEER);

    // Two peers
    const std::vector<Slot> twoslots = {{100, 100, 69}, {300, 100, 42}};

    // Undershoot
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 0, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 42, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 99, 500), NO_PEER);

    // First entry
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 100, 500), 69);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 142, 500), 69);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 199, 500), 69);

    // In between
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 200, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 242, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 299, 500), NO_PEER);

    // Second entry
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 300, 500), 42);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 342, 500), 42);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 399, 500), 42);

    // Overshoot
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 400, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 442, 500), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(twoslots, 499, 500), NO_PEER);
}

BOOST_AUTO_TEST_CASE(select_peer_dichotomic) {
    std::vector<Slot> slots;

    // 100 peers of size 1 with 1 empty element apart.
    uint64_t max = 1;
    for (int i = 0; i < 100; i++) {
        slots.emplace_back(max, 1, i);
        max += 2;
    }

    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 4, max), NO_PEER);

    // Check that we get what we expect.
    for (int i = 0; i < 100; i++) {
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 2 * i, max), NO_PEER);
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 2 * i + 1, max), i);
    }

    BOOST_CHECK_EQUAL(selectPeerImpl(slots, max, max), NO_PEER);

    // Update the slots to be heavily skewed toward the last element.
    slots[99] = slots[99].withScore(101);
    max = slots[99].getStop();
    BOOST_CHECK_EQUAL(max, 300);

    for (int i = 0; i < 100; i++) {
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 2 * i, max), NO_PEER);
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 2 * i + 1, max), i);
    }

    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 200, max), 99);
    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 256, max), 99);
    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 299, max), 99);
    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 300, max), NO_PEER);

    // Update the slots to be heavily skewed toward the first element.
    for (int i = 0; i < 100; i++) {
        slots[i] = slots[i].withStart(slots[i].getStart() + 100);
    }

    slots[0] = Slot(1, slots[0].getStop() - 1, slots[0].getPeerId());
    slots[99] = slots[99].withScore(1);
    max = slots[99].getStop();
    BOOST_CHECK_EQUAL(max, 300);

    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 0, max), NO_PEER);
    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 1, max), 0);
    BOOST_CHECK_EQUAL(selectPeerImpl(slots, 42, max), 0);

    for (int i = 0; i < 100; i++) {
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 100 + 2 * i + 1, max), i);
        BOOST_CHECK_EQUAL(selectPeerImpl(slots, 100 + 2 * i + 2, max), NO_PEER);
    }
}

BOOST_AUTO_TEST_CASE(select_peer_random) {
    for (int c = 0; c < 1000; c++) {
        size_t size = InsecureRandBits(10) + 1;
        std::vector<Slot> slots;
        slots.reserve(size);

        uint64_t max = InsecureRandBits(3);
        auto next = [&]() {
            uint64_t r = max;
            max += InsecureRandBits(3);
            return r;
        };

        for (size_t i = 0; i < size; i++) {
            const uint64_t start = next();
            const uint32_t score = InsecureRandBits(3);
            max += score;
            slots.emplace_back(start, score, i);
        }

        for (int k = 0; k < 100; k++) {
            uint64_t s = max > 0 ? InsecureRandRange(max) : 0;
            auto i = selectPeerImpl(slots, s, max);
            // /!\ Because of the way we construct the vector, the peer id is
            // always the index. This might not be the case in practice.
            BOOST_CHECK(i == NO_PEER || slots[i].contains(s));
        }
    }
}

static void addNodeWithScore(avalanche::PeerManager &pm, NodeId node,
                             uint32_t score) {
    auto proof = buildRandomProof(score);
    BOOST_CHECK(pm.registerProof(proof));
    BOOST_CHECK(pm.addNode(node, proof->getId()));
};

BOOST_AUTO_TEST_CASE(peer_probabilities) {
    // No peers.
    avalanche::PeerManager pm;
    BOOST_CHECK_EQUAL(pm.selectNode(), NO_NODE);

    const NodeId node0 = 42, node1 = 69, node2 = 37;

    // One peer, we always return it.
    addNodeWithScore(pm, node0, MIN_VALID_PROOF_SCORE);
    BOOST_CHECK_EQUAL(pm.selectNode(), node0);

    // Two peers, verify ratio.
    addNodeWithScore(pm, node1, 2 * MIN_VALID_PROOF_SCORE);

    std::unordered_map<PeerId, int> results = {};
    for (int i = 0; i < 10000; i++) {
        size_t n = pm.selectNode();
        BOOST_CHECK(n == node0 || n == node1);
        results[n]++;
    }

    BOOST_CHECK(abs(2 * results[0] - results[1]) < 500);

    // Three peers, verify ratio.
    addNodeWithScore(pm, node2, MIN_VALID_PROOF_SCORE);

    results.clear();
    for (int i = 0; i < 10000; i++) {
        size_t n = pm.selectNode();
        BOOST_CHECK(n == node0 || n == node1 || n == node2);
        results[n]++;
    }

    BOOST_CHECK(abs(results[0] - results[1] + results[2]) < 500);
}

BOOST_AUTO_TEST_CASE(remove_peer) {
    // No peers.
    avalanche::PeerManager pm;
    BOOST_CHECK_EQUAL(pm.selectPeer(), NO_PEER);

    // Add 4 peers.
    std::array<PeerId, 8> peerids;
    for (int i = 0; i < 4; i++) {
        auto p = buildRandomProof(100);
        peerids[i] = TestPeerManager::registerAndGetPeerId(pm, p);
        BOOST_CHECK(pm.addNode(InsecureRand32(), p->getId()));
    }

    BOOST_CHECK_EQUAL(pm.getSlotCount(), 400);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 0);

    for (int i = 0; i < 100; i++) {
        PeerId p = pm.selectPeer();
        BOOST_CHECK(p == peerids[0] || p == peerids[1] || p == peerids[2] ||
                    p == peerids[3]);
    }

    // Remove one peer, it nevers show up now.
    BOOST_CHECK(pm.removePeer(peerids[2]));
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 400);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 100);

    // Make sure we compact to never get NO_PEER.
    BOOST_CHECK_EQUAL(pm.compact(), 100);
    BOOST_CHECK(pm.verify());
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 300);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 0);

    for (int i = 0; i < 100; i++) {
        PeerId p = pm.selectPeer();
        BOOST_CHECK(p == peerids[0] || p == peerids[1] || p == peerids[3]);
    }

    // Add 4 more peers.
    for (int i = 0; i < 4; i++) {
        auto p = buildRandomProof(100);
        peerids[i + 4] = TestPeerManager::registerAndGetPeerId(pm, p);
        BOOST_CHECK(pm.addNode(InsecureRand32(), p->getId()));
    }

    BOOST_CHECK_EQUAL(pm.getSlotCount(), 700);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 0);

    BOOST_CHECK(pm.removePeer(peerids[0]));
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 700);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 100);

    // Removing the last entry do not increase fragmentation.
    BOOST_CHECK(pm.removePeer(peerids[7]));
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 600);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 100);

    // Make sure we compact to never get NO_PEER.
    BOOST_CHECK_EQUAL(pm.compact(), 100);
    BOOST_CHECK(pm.verify());
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 500);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 0);

    for (int i = 0; i < 100; i++) {
        PeerId p = pm.selectPeer();
        BOOST_CHECK(p == peerids[1] || p == peerids[3] || p == peerids[4] ||
                    p == peerids[5] || p == peerids[6]);
    }

    // Removing non existent peers fails.
    BOOST_CHECK(!pm.removePeer(peerids[0]));
    BOOST_CHECK(!pm.removePeer(peerids[2]));
    BOOST_CHECK(!pm.removePeer(peerids[7]));
    BOOST_CHECK(!pm.removePeer(NO_PEER));
}

BOOST_AUTO_TEST_CASE(compact_slots) {
    avalanche::PeerManager pm;

    // Add 4 peers.
    std::array<PeerId, 4> peerids;
    for (int i = 0; i < 4; i++) {
        auto p = buildRandomProof(100);
        peerids[i] = TestPeerManager::registerAndGetPeerId(pm, p);
        BOOST_CHECK(pm.addNode(InsecureRand32(), p->getId()));
    }

    // Remove all peers.
    for (auto p : peerids) {
        pm.removePeer(p);
    }

    BOOST_CHECK_EQUAL(pm.getSlotCount(), 300);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 300);

    for (int i = 0; i < 100; i++) {
        BOOST_CHECK_EQUAL(pm.selectPeer(), NO_PEER);
    }

    BOOST_CHECK_EQUAL(pm.compact(), 300);
    BOOST_CHECK(pm.verify());
    BOOST_CHECK_EQUAL(pm.getSlotCount(), 0);
    BOOST_CHECK_EQUAL(pm.getFragmentation(), 0);
}

BOOST_AUTO_TEST_CASE(node_crud) {
    avalanche::PeerManager pm;

    // Create one peer.
    auto proof = buildRandomProof(10000000 * MIN_VALID_PROOF_SCORE);
    BOOST_CHECK(pm.registerProof(proof));
    BOOST_CHECK_EQUAL(pm.selectNode(), NO_NODE);

    // Add 4 nodes.
    const ProofId &proofid = proof->getId();
    for (int i = 0; i < 4; i++) {
        BOOST_CHECK(pm.addNode(i, proofid));
    }

    for (int i = 0; i < 100; i++) {
        NodeId n = pm.selectNode();
        BOOST_CHECK(n >= 0 && n < 4);
        BOOST_CHECK(
            pm.updateNextRequestTime(n, std::chrono::steady_clock::now()));
    }

    // Remove a node, check that it doesn't show up.
    BOOST_CHECK(pm.removeNode(2));

    for (int i = 0; i < 100; i++) {
        NodeId n = pm.selectNode();
        BOOST_CHECK(n == 0 || n == 1 || n == 3);
        BOOST_CHECK(
            pm.updateNextRequestTime(n, std::chrono::steady_clock::now()));
    }

    // Push a node's timeout in the future, so that it doesn't show up.
    BOOST_CHECK(pm.updateNextRequestTime(1, std::chrono::steady_clock::now() +
                                                std::chrono::hours(24)));

    for (int i = 0; i < 100; i++) {
        NodeId n = pm.selectNode();
        BOOST_CHECK(n == 0 || n == 3);
        BOOST_CHECK(
            pm.updateNextRequestTime(n, std::chrono::steady_clock::now()));
    }

    // Move a node from a peer to another. This peer has a very low score such
    // as chances of being picked are 1 in 10 million.
    addNodeWithScore(pm, 3, MIN_VALID_PROOF_SCORE);

    int node3selected = 0;
    for (int i = 0; i < 100; i++) {
        NodeId n = pm.selectNode();
        if (n == 3) {
            // Selecting this node should be exceedingly unlikely.
            BOOST_CHECK(node3selected++ < 1);
        } else {
            BOOST_CHECK_EQUAL(n, 0);
        }
        BOOST_CHECK(
            pm.updateNextRequestTime(n, std::chrono::steady_clock::now()));
    }
}

BOOST_AUTO_TEST_CASE(node_binding) {
    avalanche::PeerManager pm;

    auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    const ProofId &proofid = proof->getId();

    BOOST_CHECK_EQUAL(pm.getNodeCount(), 0);
    BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 0);

    // Add a bunch of nodes with no associated peer
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(!pm.addNode(i, proofid));
        BOOST_CHECK(TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 0);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), i + 1);
    }

    // Now create the peer and check all the nodes are bound
    const PeerId peerid = TestPeerManager::registerAndGetPeerId(pm, proof);
    BOOST_CHECK_NE(peerid, NO_PEER);
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 10);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 0);
    }
    BOOST_CHECK(pm.verify());

    // Disconnect some nodes
    for (int i = 0; i < 5; i++) {
        BOOST_CHECK(pm.removeNode(i));
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(!TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 10 - i - 1);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 0);
    }

    // Add nodes when the peer already exists
    for (int i = 0; i < 5; i++) {
        BOOST_CHECK(pm.addNode(i, proofid));
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 5 + i + 1);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 0);
    }

    auto alt_proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    const ProofId &alt_proofid = alt_proof->getId();

    // Update some nodes from a known proof to an unknown proof
    for (int i = 0; i < 5; i++) {
        BOOST_CHECK(!pm.addNode(i, alt_proofid));
        BOOST_CHECK(TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(!TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 10 - i - 1);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), i + 1);
    }

    auto alt2_proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    const ProofId &alt2_proofid = alt2_proof->getId();

    // Update some nodes from an unknown proof to another unknown proof
    for (int i = 0; i < 5; i++) {
        BOOST_CHECK(!pm.addNode(i, alt2_proofid));
        BOOST_CHECK(TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 5);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 5);
    }

    // Update some nodes from an unknown proof to a known proof
    for (int i = 0; i < 5; i++) {
        BOOST_CHECK(pm.addNode(i, proofid));
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 5 + i + 1);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 5 - i - 1);
    }

    // Remove the peer, the nodes should be pending again
    BOOST_CHECK(pm.removePeer(peerid));
    BOOST_CHECK(!pm.exists(proof->getId()));
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(!TestPeerManager::nodeBelongToPeer(pm, i, peerid));
        BOOST_CHECK_EQUAL(pm.getNodeCount(), 0);
        BOOST_CHECK_EQUAL(pm.getPendingNodeCount(), 10);
    }
    BOOST_CHECK(pm.verify());
}

BOOST_AUTO_TEST_CASE(node_binding_reorg) {
    avalanche::PeerManager pm;

    ProofBuilder pb(0, 0, CKey::MakeCompressedKey());
    auto key = CKey::MakeCompressedKey();
    const CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));
    COutPoint utxo(TxId(GetRandHash()), 0);
    Amount amount = 1 * COIN;
    const int height = 1234;
    BOOST_CHECK(pb.addUTXO(utxo, amount, height, false, key));
    auto proof = pb.build();
    const ProofId &proofid = proof->getId();

    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(utxo, Coin(CTxOut(amount, script), height, false), false);
    }

    PeerId peerid = TestPeerManager::registerAndGetPeerId(pm, proof);
    BOOST_CHECK_NE(peerid, NO_PEER);
    BOOST_CHECK(pm.verify());

    // Add nodes to our peer
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(pm.addNode(i, proofid));
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(TestPeerManager::nodeBelongToPeer(pm, i, peerid));
    }

    // Orphan the proof
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.SpendCoin(utxo);
    }

    pm.updatedBlockTip();
    BOOST_CHECK(pm.isOrphan(proofid));
    BOOST_CHECK(!pm.isBoundToPeer(proofid));
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(!TestPeerManager::nodeBelongToPeer(pm, i, peerid));
    }
    BOOST_CHECK(pm.verify());

    // Make the proof great again
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(utxo, Coin(CTxOut(amount, script), height, false), false);
    }

    pm.updatedBlockTip();
    BOOST_CHECK(!pm.isOrphan(proofid));
    BOOST_CHECK(pm.isBoundToPeer(proofid));
    // The peerid has certainly been updated
    peerid = TestPeerManager::registerAndGetPeerId(pm, proof);
    BOOST_CHECK_NE(peerid, NO_PEER);
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(!TestPeerManager::isNodePending(pm, i));
        BOOST_CHECK(TestPeerManager::nodeBelongToPeer(pm, i, peerid));
    }
    BOOST_CHECK(pm.verify());
}

BOOST_AUTO_TEST_CASE(proof_conflict) {
    auto key = CKey::MakeCompressedKey();
    const CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    TxId txid1(GetRandHash());
    TxId txid2(GetRandHash());
    BOOST_CHECK(txid1 != txid2);

    const Amount v = 5 * COIN;
    const int height = 1234;

    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();

        for (int i = 0; i < 10; i++) {
            coins.AddCoin(COutPoint(txid1, i),
                          Coin(CTxOut(v, script), height, false), false);
            coins.AddCoin(COutPoint(txid2, i),
                          Coin(CTxOut(v, script), height, false), false);
        }
    }

    avalanche::PeerManager pm;
    CKey masterKey = CKey::MakeCompressedKey();
    const auto getPeerId = [&](const std::vector<COutPoint> &outpoints) {
        ProofBuilder pb(0, 0, masterKey);
        for (const auto &o : outpoints) {
            BOOST_CHECK(pb.addUTXO(o, v, height, false, key));
        }

        return TestPeerManager::registerAndGetPeerId(pm, pb.build());
    };

    // Add one peer.
    const PeerId peer1 = getPeerId({COutPoint(txid1, 0)});
    BOOST_CHECK(peer1 != NO_PEER);

    // Same proof, same peer.
    BOOST_CHECK_EQUAL(getPeerId({COutPoint(txid1, 0)}), peer1);

    // Different txid, different proof.
    const PeerId peer2 = getPeerId({COutPoint(txid2, 0)});
    BOOST_CHECK(peer2 != NO_PEER && peer2 != peer1);

    // Different index, different proof.
    const PeerId peer3 = getPeerId({COutPoint(txid1, 1)});
    BOOST_CHECK(peer3 != NO_PEER && peer3 != peer1);

    // Empty proof, no peer.
    BOOST_CHECK_EQUAL(getPeerId({}), NO_PEER);

    // Multiple inputs.
    const PeerId peer4 = getPeerId({COutPoint(txid1, 2), COutPoint(txid2, 2)});
    BOOST_CHECK(peer4 != NO_PEER && peer4 != peer1);

    // Duplicated input.
    {
        ProofBuilder pb(0, 0, CKey::MakeCompressedKey());
        COutPoint o(txid1, 3);
        BOOST_CHECK(pb.addUTXO(o, v, height, false, key));
        BOOST_CHECK(
            !pm.registerProof(TestProofBuilder::buildDuplicatedStakes(pb)));
    }

    // Multiple inputs, collision on first input.
    BOOST_CHECK_EQUAL(getPeerId({COutPoint(txid1, 0), COutPoint(txid2, 4)}),
                      NO_PEER);

    // Mutliple inputs, collision on second input.
    BOOST_CHECK_EQUAL(getPeerId({COutPoint(txid1, 4), COutPoint(txid2, 0)}),
                      NO_PEER);

    // Mutliple inputs, collision on both inputs.
    BOOST_CHECK_EQUAL(getPeerId({COutPoint(txid1, 0), COutPoint(txid2, 2)}),
                      NO_PEER);
}

BOOST_AUTO_TEST_CASE(orphan_proofs) {
    avalanche::PeerManager pm;

    auto key = CKey::MakeCompressedKey();
    const CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    COutPoint outpoint1 = COutPoint(TxId(GetRandHash()), 0);
    COutPoint outpoint2 = COutPoint(TxId(GetRandHash()), 0);
    COutPoint outpoint3 = COutPoint(TxId(GetRandHash()), 0);

    const Amount v = 5 * COIN;
    const int height = 1234;
    const int wrongHeight = 12345;

    const auto makeProof = [&](const COutPoint &outpoint, const int h) {
        ProofBuilder pb(0, 0, CKey::MakeCompressedKey());
        BOOST_CHECK(pb.addUTXO(outpoint, v, h, false, key));
        return pb.build();
    };

    auto proof1 = makeProof(outpoint1, height);
    auto proof2 = makeProof(outpoint2, height);
    auto proof3 = makeProof(outpoint3, wrongHeight);

    const Coin coin = Coin(CTxOut(v, script), height, false);

    // Add outpoints 1 and 3, not 2
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(outpoint1, coin, false);
        coins.AddCoin(outpoint3, coin, false);
    }

    // Add the proofs
    BOOST_CHECK(pm.registerProof(proof1));

    auto registerOrphan = [&](const ProofRef &proof) {
        ProofRegistrationState state;
        BOOST_CHECK(!pm.registerProof(proof, state));
        BOOST_CHECK(state.GetResult() == ProofRegistrationResult::ORPHAN);
    };

    registerOrphan(proof2);
    registerOrphan(proof3);

    auto checkOrphan = [&](const ProofRef &proof, bool expectedOrphan) {
        const ProofId &proofid = proof->getId();
        BOOST_CHECK(pm.exists(proofid));

        BOOST_CHECK_EQUAL(pm.isOrphan(proofid), expectedOrphan);
        BOOST_CHECK_EQUAL(pm.isBoundToPeer(proofid), !expectedOrphan);

        bool ret = false;
        pm.forEachPeer([&](const Peer &peer) {
            if (proof->getId() == peer.proof->getId()) {
                ret = true;
            }
        });
        BOOST_CHECK_EQUAL(ret, !expectedOrphan);
    };

    // Good
    checkOrphan(proof1, false);
    // MISSING_UTXO
    checkOrphan(proof2, true);
    // HEIGHT_MISMATCH
    checkOrphan(proof3, true);

    // Add outpoint2, proof2 is no longer considered orphan
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(outpoint2, coin, false);
    }

    pm.updatedBlockTip();
    checkOrphan(proof2, false);

    // The status of proof1 and proof3 are unchanged
    checkOrphan(proof1, false);
    checkOrphan(proof3, true);

    // Spend outpoint1, proof1 becomes orphan
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.SpendCoin(outpoint1);
    }

    pm.updatedBlockTip();
    checkOrphan(proof1, true);

    // The status of proof2 and proof3 are unchanged
    checkOrphan(proof2, false);
    checkOrphan(proof3, true);

    // A reorg could make a previous HEIGHT_MISMATCH become valid
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.SpendCoin(outpoint3);
        coins.AddCoin(outpoint3, Coin(CTxOut(v, script), wrongHeight, false),
                      false);
    }

    pm.updatedBlockTip();
    checkOrphan(proof3, false);

    // The status of proof 1 and proof2 are unchanged
    checkOrphan(proof1, true);
    checkOrphan(proof2, false);
}

BOOST_AUTO_TEST_CASE(dangling_node) {
    avalanche::PeerManager pm;

    auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    PeerId peerid = TestPeerManager::registerAndGetPeerId(pm, proof);
    BOOST_CHECK_NE(peerid, NO_PEER);

    const TimePoint theFuture(std::chrono::steady_clock::now() +
                              std::chrono::hours(24));

    // Add nodes to this peer and update their request time far in the future
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(pm.addNode(i, proof->getId()));
        BOOST_CHECK(pm.updateNextRequestTime(i, theFuture));
    }

    // Remove the peer
    BOOST_CHECK(pm.removePeer(peerid));

    // Check the nodes are still there
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(pm.forNode(i, [](const Node &n) { return true; }));
    }

    // Build a new one
    proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    peerid = TestPeerManager::registerAndGetPeerId(pm, proof);
    BOOST_CHECK_NE(peerid, NO_PEER);

    // Update the nodes with the new proof
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(pm.addNode(i, proof->getId()));
        BOOST_CHECK(pm.forNode(
            i, [&](const Node &n) { return n.nextRequestTime == theFuture; }));
    }

    // Remove the peer
    BOOST_CHECK(pm.removePeer(peerid));

    // Disconnect the nodes
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(pm.removeNode(i));
    }
}

BOOST_AUTO_TEST_CASE(proof_accessors) {
    avalanche::PeerManager pm;

    constexpr int numProofs = 10;

    std::vector<ProofRef> proofs;
    proofs.reserve(numProofs);
    for (int i = 0; i < numProofs; i++) {
        proofs.push_back(buildRandomProof(MIN_VALID_PROOF_SCORE));
    }

    for (int i = 0; i < numProofs; i++) {
        BOOST_CHECK(pm.registerProof(proofs[i]));

        {
            ProofRegistrationState state;
            // Fail to add an existing proof
            BOOST_CHECK(!pm.registerProof(proofs[i], state));
            BOOST_CHECK(state.GetResult() ==
                        ProofRegistrationResult::ALREADY_REGISTERED);
        }

        for (int added = 0; added <= i; added++) {
            auto proof = pm.getProof(proofs[added]->getId());
            BOOST_CHECK(proof != nullptr);

            const ProofId &proofid = proof->getId();
            BOOST_CHECK_EQUAL(proofid, proofs[added]->getId());
        }
    }

    // No stake, copied from proof_tests.cpp
    const std::string badProofHex(
        "96527eae083f1f24625f049d9e54bb9a2102a93d98bf42ab90cfc0bf9e7c634ed76a7"
        "3e95b02cacfd357b64e4fb6c92e92dd00");
    bilingual_str error;
    Proof badProof;
    BOOST_CHECK(Proof::FromHex(badProof, badProofHex, error));

    ProofRegistrationState state;
    BOOST_CHECK(
        !pm.registerProof(std::make_shared<Proof>(std::move(badProof)), state));
    BOOST_CHECK(state.GetResult() == ProofRegistrationResult::INVALID);
}

BOOST_FIXTURE_TEST_CASE(conflicting_proof_rescan, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount = 10 * COIN;
    const uint32_t height = 100;
    const bool is_coinbase = false;

    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    auto addCoin = [&]() {
        LOCK(cs_main);
        COutPoint outpoint(TxId(GetRandHash()), 0);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(outpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);

        return outpoint;
    };

    const COutPoint conflictingOutpoint = addCoin();
    const COutPoint outpointToSend = addCoin();

    ProofRef proofToInvalidate;
    {
        ProofBuilder pb(0, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));
        BOOST_CHECK(
            pb.addUTXO(outpointToSend, amount, height, is_coinbase, key));
        proofToInvalidate = pb.build();
    }

    BOOST_CHECK(pm.registerProof(proofToInvalidate));

    ProofRef conflictingProof;
    {
        ProofBuilder pb(0, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));
        BOOST_CHECK(pb.addUTXO(addCoin(), amount, height, is_coinbase, key));
        conflictingProof = pb.build();
    }

    ProofRegistrationState state;
    BOOST_CHECK(!pm.registerProof(conflictingProof, state));
    BOOST_CHECK(state.GetResult() == ProofRegistrationResult::CONFLICTING);
    BOOST_CHECK(pm.isInConflictingPool(conflictingProof->getId()));

    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        // Make proofToInvalidate invalid
        coins.SpendCoin(outpointToSend);
    }

    pm.updatedBlockTip();

    BOOST_CHECK(pm.isOrphan(proofToInvalidate->getId()));

    BOOST_CHECK(!pm.isInConflictingPool(conflictingProof->getId()));
    BOOST_CHECK(pm.isBoundToPeer(conflictingProof->getId()));
}

BOOST_FIXTURE_TEST_CASE(conflicting_proof_selection, NoCoolDownFixture) {
    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;

    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    auto addCoin = [&](const Amount &amount) {
        LOCK(cs_main);
        const COutPoint outpoint(TxId(GetRandHash()), 0);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(outpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
        return outpoint;
    };

    // This will be the conflicting UTXO for all the following proofs
    auto conflictingOutpoint = addCoin(amount);

    auto buildProofWithSequence = [&](uint64_t sequence) {
        ProofBuilder pb(sequence, GetRandInt(std::numeric_limits<int>::max()),
                        key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));

        return pb.build();
    };

    auto proof_base = buildProofWithSequence(10);

    gArgs.ForceSetArg("-enableavalancheproofreplacement", "1");

    ConflictingProofComparator comparator;
    auto checkPreferred = [&](const ProofRef &candidate,
                              const ProofRef &reference, bool expectAccepted) {
        BOOST_CHECK_EQUAL(comparator(candidate, reference), expectAccepted);
        BOOST_CHECK_EQUAL(comparator(reference, candidate), !expectAccepted);

        avalanche::PeerManager pm;
        BOOST_CHECK(pm.registerProof(reference));
        BOOST_CHECK(pm.isBoundToPeer(reference->getId()));

        ProofRegistrationState state;
        BOOST_CHECK_EQUAL(pm.registerProof(candidate, state), expectAccepted);
        BOOST_CHECK_EQUAL(state.IsValid(), expectAccepted);
        BOOST_CHECK_EQUAL(state.GetResult() ==
                              ProofRegistrationResult::CONFLICTING,
                          !expectAccepted);

        BOOST_CHECK_EQUAL(pm.isBoundToPeer(candidate->getId()), expectAccepted);
        BOOST_CHECK_EQUAL(pm.isInConflictingPool(candidate->getId()),
                          !expectAccepted);

        BOOST_CHECK_EQUAL(pm.isBoundToPeer(reference->getId()),
                          !expectAccepted);
        BOOST_CHECK_EQUAL(pm.isInConflictingPool(reference->getId()),
                          expectAccepted);
    };

    // Same master key, lower sequence number
    checkPreferred(buildProofWithSequence(9), proof_base, false);
    // Same master key, higher sequence number
    checkPreferred(buildProofWithSequence(11), proof_base, true);

    auto buildProofFromAmounts = [&](const CKey &master,
                                     std::vector<Amount> &&amounts) {
        ProofBuilder pb(0, 0, master);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));
        for (const Amount &v : amounts) {
            auto outpoint = addCoin(v);
            BOOST_CHECK(
                pb.addUTXO(std::move(outpoint), v, height, is_coinbase, key));
        }
        return pb.build();
    };

    auto proof_multiUtxo = buildProofFromAmounts(key, {10 * COIN, 10 * COIN});

    // Test for both the same master and a different one. The sequence number
    // is the same for all these tests.
    for (const CKey &k : {key, CKey::MakeCompressedKey()}) {
        // Low amount
        checkPreferred(buildProofFromAmounts(k, {10 * COIN, 5 * COIN}),
                       proof_multiUtxo, false);
        // High amount
        checkPreferred(buildProofFromAmounts(k, {10 * COIN, 15 * COIN}),
                       proof_multiUtxo, true);
        // Same amount, low stake count
        checkPreferred(buildProofFromAmounts(k, {20 * COIN}), proof_multiUtxo,
                       true);
        // Same amount, high stake count
        checkPreferred(
            buildProofFromAmounts(k, {10 * COIN, 5 * COIN, 5 * COIN}),
            proof_multiUtxo, false);
        // Same amount, same stake count, selection is done on proof id
        auto proofSimilar = buildProofFromAmounts(k, {10 * COIN, 10 * COIN});
        checkPreferred(proofSimilar, proof_multiUtxo,
                       proofSimilar->getId() < proof_multiUtxo->getId());
    }

    gArgs.ClearForcedArg("-enableavalancheproofreplacement");
}

BOOST_AUTO_TEST_CASE(conflicting_orphans) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    auto buildProofWithSequence = [&](uint64_t sequence,
                                      const std::vector<COutPoint> &outpoints) {
        ProofBuilder pb(sequence, 0, key);

        for (const COutPoint &outpoint : outpoints) {
            BOOST_CHECK(pb.addUTXO(outpoint, amount, height, is_coinbase, key));
        }

        return pb.build();
    };

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    const COutPoint randomOutpoint1(TxId(GetRandHash()), 0);

    auto orphan10 = buildProofWithSequence(10, {conflictingOutpoint});
    auto orphan20 =
        buildProofWithSequence(20, {conflictingOutpoint, randomOutpoint1});

    BOOST_CHECK(!pm.registerProof(orphan10));
    BOOST_CHECK(pm.isOrphan(orphan10->getId()));

    BOOST_CHECK(!pm.registerProof(orphan20));
    BOOST_CHECK(pm.isOrphan(orphan20->getId()));
    BOOST_CHECK(!pm.exists(orphan10->getId()));

    auto addCoin = [&](const COutPoint &outpoint) {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(outpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    };

    const COutPoint outpointToSend(TxId(GetRandHash()), 0);
    // Add both randomOutpoint1 and outpointToSend to the UTXO set. The orphan20
    // proof is still an orphan because the conflictingOutpoint is unknown.
    addCoin(randomOutpoint1);
    addCoin(outpointToSend);

    // Build and register proof valid proof that will conflict with the orphan
    auto proof30 =
        buildProofWithSequence(30, {randomOutpoint1, outpointToSend});
    BOOST_CHECK(pm.registerProof(proof30));
    BOOST_CHECK(pm.isBoundToPeer(proof30->getId()));

    // Spend the outpointToSend to orphan proof30
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.SpendCoin(outpointToSend);
    }

    // Check that a rescan will also select the preferred orphan, in this case
    // proof30 will replace orphan20.
    pm.updatedBlockTip();

    BOOST_CHECK(!pm.isBoundToPeer(proof30->getId()));
    BOOST_CHECK(pm.isOrphan(proof30->getId()));
    BOOST_CHECK(!pm.exists(orphan20->getId()));
}

BOOST_FIXTURE_TEST_CASE(preferred_conflicting_proof, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    }

    auto buildProofWithSequence = [&](uint64_t sequence) {
        ProofBuilder pb(sequence, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));

        return pb.build();
    };

    auto proofSeq10 = buildProofWithSequence(10);
    auto proofSeq20 = buildProofWithSequence(20);
    auto proofSeq30 = buildProofWithSequence(30);

    BOOST_CHECK(pm.registerProof(proofSeq30));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
    BOOST_CHECK(!pm.isInConflictingPool(proofSeq30->getId()));

    // proofSeq10 is a worst candidate than proofSeq30, so it goes to the
    // conflicting pool.
    BOOST_CHECK(!pm.registerProof(proofSeq10));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
    BOOST_CHECK(!pm.isBoundToPeer(proofSeq10->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq10->getId()));

    // proofSeq20 is a worst candidate than proofSeq30 but a better one than
    // proogSeq10, so it replaces it in the conflicting pool and proofSeq10 is
    // evicted.
    BOOST_CHECK(!pm.registerProof(proofSeq20));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
    BOOST_CHECK(!pm.isBoundToPeer(proofSeq20->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq20->getId()));
    BOOST_CHECK(!pm.exists(proofSeq10->getId()));
}

BOOST_FIXTURE_TEST_CASE(update_next_conflict_time, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    auto now = GetTime<std::chrono::seconds>();
    SetMockTime(now.count());

    // Updating the time of an unknown peer should fail
    for (size_t i = 0; i < 10; i++) {
        BOOST_CHECK(
            !pm.updateNextPossibleConflictTime(PeerId(GetRandInt(1000)), now));
    }

    auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    PeerId peerid = TestPeerManager::registerAndGetPeerId(pm, proof);

    auto checkNextPossibleConflictTime = [&](std::chrono::seconds expected) {
        BOOST_CHECK(pm.forPeer(proof->getId(), [&](const Peer &p) {
            return p.nextPossibleConflictTime == expected;
        }));
    };

    checkNextPossibleConflictTime(now);

    // Move the time in the past is not possible
    BOOST_CHECK(!pm.updateNextPossibleConflictTime(
        peerid, now - std::chrono::seconds{1}));
    checkNextPossibleConflictTime(now);

    BOOST_CHECK(pm.updateNextPossibleConflictTime(
        peerid, now + std::chrono::seconds{1}));
    checkNextPossibleConflictTime(now + std::chrono::seconds{1});
}

BOOST_FIXTURE_TEST_CASE(register_force_accept, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    }

    auto buildProofWithSequence = [&](uint64_t sequence) {
        ProofBuilder pb(sequence, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));

        return pb.build();
    };

    auto proofSeq10 = buildProofWithSequence(10);
    auto proofSeq20 = buildProofWithSequence(20);
    auto proofSeq30 = buildProofWithSequence(30);

    BOOST_CHECK(pm.registerProof(proofSeq30));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
    BOOST_CHECK(!pm.isInConflictingPool(proofSeq30->getId()));

    // proofSeq20 is a worst candidate than proofSeq30, so it goes to the
    // conflicting pool.
    BOOST_CHECK(!pm.registerProof(proofSeq20));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq20->getId()));

    // We can force the acceptance of proofSeq20
    using RegistrationMode = avalanche::PeerManager::RegistrationMode;
    BOOST_CHECK(pm.registerProof(proofSeq20, RegistrationMode::FORCE_ACCEPT));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq20->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq30->getId()));

    // We can also force the acceptance of a proof which is not already in the
    // conflicting pool.
    BOOST_CHECK(!pm.registerProof(proofSeq10));
    BOOST_CHECK(!pm.exists(proofSeq10->getId()));

    BOOST_CHECK(pm.registerProof(proofSeq10, RegistrationMode::FORCE_ACCEPT));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq10->getId()));
    BOOST_CHECK(!pm.exists(proofSeq20->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq30->getId()));

    // Attempting to register again fails, and has no impact on the pools
    for (size_t i = 0; i < 10; i++) {
        BOOST_CHECK(!pm.registerProof(proofSeq10));
        BOOST_CHECK(
            !pm.registerProof(proofSeq10, RegistrationMode::FORCE_ACCEPT));

        BOOST_CHECK(pm.isBoundToPeer(proofSeq10->getId()));
        BOOST_CHECK(!pm.exists(proofSeq20->getId()));
        BOOST_CHECK(pm.isInConflictingPool(proofSeq30->getId()));
    }

    // Revert between proofSeq10 and proofSeq30 a few times
    for (size_t i = 0; i < 10; i++) {
        BOOST_CHECK(
            pm.registerProof(proofSeq30, RegistrationMode::FORCE_ACCEPT));

        BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));
        BOOST_CHECK(pm.isInConflictingPool(proofSeq10->getId()));

        BOOST_CHECK(
            pm.registerProof(proofSeq10, RegistrationMode::FORCE_ACCEPT));

        BOOST_CHECK(pm.isBoundToPeer(proofSeq10->getId()));
        BOOST_CHECK(pm.isInConflictingPool(proofSeq30->getId()));
    }
}

BOOST_FIXTURE_TEST_CASE(evicted_proof, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    }

    auto buildProofWithSequence = [&](uint64_t sequence) {
        ProofBuilder pb(sequence, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));
        return pb.build();
    };

    auto proofSeq10 = buildProofWithSequence(10);
    auto proofSeq20 = buildProofWithSequence(20);
    auto proofSeq30 = buildProofWithSequence(30);

    {
        ProofRegistrationState state;
        BOOST_CHECK(pm.registerProof(proofSeq30, state));
        BOOST_CHECK(state.IsValid());
    }

    {
        ProofRegistrationState state;
        BOOST_CHECK(!pm.registerProof(proofSeq20, state));
        BOOST_CHECK(state.GetResult() == ProofRegistrationResult::CONFLICTING);
    }

    {
        ProofRegistrationState state;
        BOOST_CHECK(!pm.registerProof(proofSeq10, state));
        BOOST_CHECK(state.GetResult() == ProofRegistrationResult::REJECTED);
    }
}

BOOST_AUTO_TEST_CASE(conflicting_proof_cooldown) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    }

    auto buildProofWithSequence = [&](uint64_t sequence) {
        ProofBuilder pb(sequence, 0, key);
        BOOST_CHECK(
            pb.addUTXO(conflictingOutpoint, amount, height, is_coinbase, key));
        return pb.build();
    };

    auto proofSeq20 = buildProofWithSequence(20);
    auto proofSeq30 = buildProofWithSequence(30);
    auto proofSeq40 = buildProofWithSequence(40);

    int64_t conflictingProofCooldown = 100;
    gArgs.ForceSetArg("-avalancheconflictingproofcooldown",
                      strprintf("%d", conflictingProofCooldown));

    int64_t now = GetTime();

    auto increaseMockTime = [&](int64_t s) {
        now += s;
        SetMockTime(now);
    };
    increaseMockTime(0);

    BOOST_CHECK(pm.registerProof(proofSeq30));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq30->getId()));

    auto checkRegistrationFailure = [&](const ProofRef &proof,
                                        ProofRegistrationResult reason) {
        ProofRegistrationState state;
        BOOST_CHECK(!pm.registerProof(proof, state));
        BOOST_CHECK(state.GetResult() == reason);
    };

    // Registering a conflicting proof will fail due to the conflicting proof
    // cooldown
    checkRegistrationFailure(proofSeq20,
                             ProofRegistrationResult::COOLDOWN_NOT_ELAPSED);
    BOOST_CHECK(!pm.exists(proofSeq20->getId()));

    // The cooldown applies as well if the proof is the favorite
    checkRegistrationFailure(proofSeq40,
                             ProofRegistrationResult::COOLDOWN_NOT_ELAPSED);
    BOOST_CHECK(!pm.exists(proofSeq40->getId()));

    // Elapse the cooldown
    increaseMockTime(conflictingProofCooldown);

    // The proof will now be added to conflicting pool
    checkRegistrationFailure(proofSeq20, ProofRegistrationResult::CONFLICTING);
    BOOST_CHECK(pm.isInConflictingPool(proofSeq20->getId()));

    // But no other
    checkRegistrationFailure(proofSeq40,
                             ProofRegistrationResult::COOLDOWN_NOT_ELAPSED);
    BOOST_CHECK(!pm.exists(proofSeq40->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq20->getId()));

    // Elapse the cooldown
    increaseMockTime(conflictingProofCooldown);

    // The proof will now be added to conflicting pool
    checkRegistrationFailure(proofSeq40, ProofRegistrationResult::CONFLICTING);
    BOOST_CHECK(pm.isInConflictingPool(proofSeq40->getId()));
    BOOST_CHECK(!pm.exists(proofSeq20->getId()));

    gArgs.ClearForcedArg("-avalancheconflictingproofcooldown");
}

BOOST_FIXTURE_TEST_CASE(reject_proof, NoCoolDownFixture) {
    avalanche::PeerManager pm;

    const CKey key = CKey::MakeCompressedKey();

    const Amount amount(10 * COIN);
    const uint32_t height = 100;
    const bool is_coinbase = false;
    CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

    const COutPoint conflictingOutpoint(TxId(GetRandHash()), 0);
    {
        LOCK(cs_main);
        CCoinsViewCache &coins = ::ChainstateActive().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(amount, script), height, is_coinbase), false);
    }

    auto buildProofWithSequenceAndOutpoints =
        [&](uint64_t sequence, const std::vector<COutPoint> &outpoints) {
            ProofBuilder pb(sequence, 0, key);
            for (const COutPoint &outpoint : outpoints) {
                BOOST_CHECK(
                    pb.addUTXO(outpoint, amount, height, is_coinbase, key));
            }
            return pb.build();
        };

    // The good, the bad and the ugly
    auto proofSeq10 =
        buildProofWithSequenceAndOutpoints(10, {conflictingOutpoint});
    auto proofSeq20 =
        buildProofWithSequenceAndOutpoints(20, {conflictingOutpoint});
    auto orphan30 = buildProofWithSequenceAndOutpoints(
        30, {conflictingOutpoint, {TxId(GetRandHash()), 0}});

    BOOST_CHECK(pm.registerProof(proofSeq20));
    BOOST_CHECK(!pm.registerProof(proofSeq10));
    BOOST_CHECK(!pm.registerProof(orphan30));

    BOOST_CHECK(pm.isBoundToPeer(proofSeq20->getId()));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq10->getId()));
    BOOST_CHECK(pm.isOrphan(orphan30->getId()));

    // Rejecting a proof that doesn't exist should fail
    for (size_t i = 0; i < 10; i++) {
        BOOST_CHECK(
            !pm.rejectProof(avalanche::ProofId(GetRandHash()),
                            avalanche::PeerManager::RejectionMode::DEFAULT));
        BOOST_CHECK(
            !pm.rejectProof(avalanche::ProofId(GetRandHash()),
                            avalanche::PeerManager::RejectionMode::INVALIDATE));
    }

    auto checkRejectDefault = [&](const ProofId &proofid) {
        BOOST_CHECK(pm.exists(proofid));
        const bool isOrphan = pm.isOrphan(proofid);
        BOOST_CHECK(pm.rejectProof(
            proofid, avalanche::PeerManager::RejectionMode::DEFAULT));
        BOOST_CHECK(!pm.isBoundToPeer(proofid));
        BOOST_CHECK_EQUAL(pm.exists(proofid), !isOrphan);
    };

    auto checkRejectInvalidate = [&](const ProofId &proofid) {
        BOOST_CHECK(pm.exists(proofid));
        BOOST_CHECK(pm.rejectProof(
            proofid, avalanche::PeerManager::RejectionMode::INVALIDATE));
    };

    // Reject from the orphan pool
    checkRejectDefault(orphan30->getId());
    BOOST_CHECK(!pm.registerProof(orphan30));
    BOOST_CHECK(pm.isOrphan(orphan30->getId()));
    checkRejectInvalidate(orphan30->getId());

    // Reject from the conflicting pool
    checkRejectDefault(proofSeq10->getId());
    checkRejectInvalidate(proofSeq10->getId());

    // Add again a proof to the conflicting pool
    BOOST_CHECK(!pm.registerProof(proofSeq10));
    BOOST_CHECK(pm.isInConflictingPool(proofSeq10->getId()));

    // Reject from the valid pool, default mode
    checkRejectDefault(proofSeq20->getId());

    // The conflicting proof should be promoted to a peer
    BOOST_CHECK(!pm.isInConflictingPool(proofSeq10->getId()));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq10->getId()));

    // Reject from the valid pool, invalidate mode
    checkRejectInvalidate(proofSeq10->getId());

    // The conflicting proof should also be promoted to a peer
    BOOST_CHECK(!pm.isInConflictingPool(proofSeq20->getId()));
    BOOST_CHECK(pm.isBoundToPeer(proofSeq20->getId()));
}

BOOST_AUTO_TEST_CASE(should_request_more_nodes) {
    avalanche::PeerManager pm;

    auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
    BOOST_CHECK(pm.registerProof(proof));

    // We have no nodes, so select node will fail and flag that we need more
    // nodes
    BOOST_CHECK_EQUAL(pm.selectNode(), NO_NODE);
    BOOST_CHECK(pm.shouldRequestMoreNodes());

    for (size_t i = 0; i < 10; i++) {
        // The flag will not trigger again until we fail to select nodes again
        BOOST_CHECK(!pm.shouldRequestMoreNodes());
    }

    // Add a few nodes.
    const ProofId &proofid = proof->getId();
    for (size_t i = 0; i < 10; i++) {
        BOOST_CHECK(pm.addNode(i, proofid));
    }

    auto cooldownTimepoint = std::chrono::steady_clock::now() + 10s;

    // All the nodes can be selected once
    for (size_t i = 0; i < 10; i++) {
        NodeId selectedId = pm.selectNode();
        BOOST_CHECK_NE(selectedId, NO_NODE);
        BOOST_CHECK(pm.updateNextRequestTime(selectedId, cooldownTimepoint));
        BOOST_CHECK(!pm.shouldRequestMoreNodes());
    }

    // All the nodes have been requested, next select will fail and the flag
    // should trigger
    BOOST_CHECK_EQUAL(pm.selectNode(), NO_NODE);
    BOOST_CHECK(pm.shouldRequestMoreNodes());

    for (size_t i = 0; i < 10; i++) {
        // The flag will not trigger again until we fail to select nodes again
        BOOST_CHECK(!pm.shouldRequestMoreNodes());
    }

    // Make it possible to request a node again
    BOOST_CHECK(pm.updateNextRequestTime(0, std::chrono::steady_clock::now()));
    BOOST_CHECK_NE(pm.selectNode(), NO_NODE);
    BOOST_CHECK(!pm.shouldRequestMoreNodes());
}

BOOST_AUTO_TEST_CASE(score_ordering) {
    avalanche::PeerManager pm;

    std::vector<uint32_t> expectedScores(10);
    // Expect the peers to be ordered by descending score
    std::generate(expectedScores.rbegin(), expectedScores.rend(),
                  [n = 1]() mutable { return n++ * MIN_VALID_PROOF_SCORE; });

    std::vector<ProofRef> proofs;
    proofs.reserve(expectedScores.size());
    for (uint32_t score : expectedScores) {
        proofs.push_back(buildRandomProof(score));
    }

    // Shuffle the proofs so they are registered in a random score order
    Shuffle(proofs.begin(), proofs.end(), FastRandomContext());
    for (auto &proof : proofs) {
        BOOST_CHECK(pm.registerProof(proof));
    }

    auto peersScores = TestPeerManager::getOrderedScores(pm);
    BOOST_CHECK_EQUAL_COLLECTIONS(peersScores.begin(), peersScores.end(),
                                  expectedScores.begin(), expectedScores.end());
}

BOOST_AUTO_TEST_SUITE_END()
