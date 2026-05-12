#include "eval.h"

#include "bitboard.h"
#include "endgame.h"
#include "eval_params.h"
#include "material_hash.h"
#include "pawn_hash.h"
#include "zobrist.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

// Material values used by SEE and move ordering (MG values, king kept large for SEE)
const int PieceValue[] = {0, 198, 817, 836, 1270, 2521, 20000};

// Game phase increments per piece type (max total = 24)
static const int GamePhaseInc[] = {0, 0, 1, 1, 2, 4, 0};

// Piece-square table pointers, filled once at init time from evalParams so
// the PST lookup stays a direct array read even though the underlying
// values are tuner-mutable.
static const Score *PST[7] = {};

static void ensureEvalInit() {
    static const bool initialized = []() {
        zobrist::init();
        initBitboards();
        PST[Pawn] = evalParams.PawnPST;
        PST[Knight] = evalParams.KnightPST;
        PST[Bishop] = evalParams.BishopPST;
        PST[Rook] = evalParams.RookPST;
        PST[Queen] = evalParams.QueenPST;
        PST[King] = evalParams.KingPST;
        Endgame::init();
        return true;
    }();

    (void)initialized;
}

// clang-format off

// Space evaluation gating: count safe central squares on our own side of
// the board; Stockfish weights the result quadratically by non-pawn piece
// count so the term fades in the endgame. Gated on a minimum piece count
// to keep the bonus from firing in thin positions. These stay
// non-tunable because they define the term's gating rather than a weight
// that coordinate descent could usefully adjust.
static const int SpaceWeightDivisor = 16;
static const int SpaceMinPieceCount = 2;

// Structural divisors and clamps for the king-danger to taper mapping.
// The mg half of the penalty is quadratic (danger^2 / KingDangerDivMg);
// the eg half stays linear (danger / KingDangerDivEg) because deep
// endgames rarely produce the runaway attack shapes the quadratic is
// designed to punish. KingDangerMgCap bounds the quadratic input so
// contrived positions with many safe-check squares cannot push the
// penalty past the magnitude the old attack-unit curve used to emit.
// Kept non-tunable following the SpaceWeightDivisor pattern: these set
// the shape of the curve, while the per-term weights carry the SPSA
// signal.
static const int KingDangerDivMg = 32;
static const int KingDangerDivEg = 8;
static const int KingDangerMgCap = 240;
static const int KingDangerEgCap = 96;

// Stockfish-lineage quadratic imbalance tables. Indexed by [pt1][pt2] with
// pt in { BishopPair=0, Pawn=1, Knight=2, Bishop=3, Rook=4, Queen=5 }. The
// "ours" table captures synergy between own-side pieces (e.g. knight value
// rising with own pawn count); the "theirs" table captures pressure from
// enemy pieces. Only the lower triangle is used; the upper triangle stays
// at zero so the inner loop can cap at pt2 <= pt1.
static const int QuadraticOurs[6][6] = {
    // BPair,  P,    N,    B,    R,    Q
    { 1438,    0,    0,    0,    0,    0 }, // BishopPair
    {   40,   38,    0,    0,    0,    0 }, // Pawn
    {   32,  255,  -62,    0,    0,    0 }, // Knight
    {    0,  104,    4,    0,    0,    0 }, // Bishop
    {  -26,   -2,   47,  105, -208,    0 }, // Rook
    { -189,   24,  117,  133, -134,   -6 }, // Queen
};

static const int QuadraticTheirs[6][6] = {
    // BPair,  P,    N,    B,    R,    Q
    {    0,    0,    0,    0,    0,    0 }, // BishopPair
    {   36,    0,    0,    0,    0,    0 }, // Pawn
    {    9,   63,    0,    0,    0,    0 }, // Knight
    {   59,   65,   42,    0,    0,    0 }, // Bishop
    {   46,   39,   24,  -24,    0,    0 }, // Rook
    {   97,  100,  -42,  137,  268,    0 }, // Queen
};

// clang-format on

// Evaluate the quadratic imbalance for side `us`. `pc[c][0]` carries a
// synthetic bishop-pair count (0 or 1); `pc[c][1..5]` mirror the piece
// counts for pawns through queens in the same order as the tables above.
// The /16 divisor matches Stockfish's convention.
static int imbalance(const int pc[2][6], int us) {
    int them = us ^ 1;
    int bonus = 0;
    for (int pt1 = 0; pt1 < 6; pt1++) {
        if (!pc[us][pt1]) continue;
        int v = 0;
        for (int pt2 = 0; pt2 <= pt1; pt2++) {
            v += QuadraticOurs[pt1][pt2] * pc[us][pt2];
            v += QuadraticTheirs[pt1][pt2] * pc[them][pt2];
        }
        bonus += pc[us][pt1] * v;
    }
    return bonus / 16;
}

// Per-evaluation derived context: precomputed attack maps and mobility
// area bitboards shared across every piece-activity term. `attackedBy`
// is indexed by [color][piece type] (King slot populated for completeness).
// `attackedBy2` is the squares attacked by at least two of that color's
// pieces, which threats and later king-safety terms consume directly.
struct EvalContext {
    Bitboard pawnAttacks[2];
    Bitboard mobilityArea[2];
    Bitboard attackedBy[2][7];
    Bitboard attackedBy2[2];
    Bitboard allAttacks[2];
};

static inline Bitboard pawnAttacksBB(Bitboard pawns, Color c) {
    if (c == White) {
        return ((pawns & ~FileABB) << 7) | ((pawns & ~FileHBB) << 9);
    }
    return ((pawns & ~FileABB) >> 9) | ((pawns & ~FileHBB) >> 7);
}

// Populate per-side attack maps by piece type plus the aggregate
// allAttacks and attackedBy2 bitboards. attackedBy2 must track squares
// hit by two or more distinct pieces of the same color -- including
// two pieces of the same type (for example two rooks converging on
// the enemy queen) -- so it is accumulated per individual piece
// rather than per piece-type union.
static void buildAttackMaps(const Board &board, EvalContext &ctx) {
    Bitboard occ = board.occupied;
    for (int c = 0; c < 2; c++) {
        for (int pt = 0; pt < 7; pt++)
            ctx.attackedBy[c][pt] = 0;

        Bitboard all = 0;
        Bitboard two = 0;

        // Pawns: split the capture halves so squares hit by both
        // diagonals land in attackedBy2 from the first step.
        Bitboard pawns = board.byPiece[Pawn] & board.byColor[c];
        Bitboard pawnLeft, pawnRight;
        if (c == White) {
            pawnLeft = (pawns & ~FileABB) << 7;
            pawnRight = (pawns & ~FileHBB) << 9;
        } else {
            pawnLeft = (pawns & ~FileABB) >> 9;
            pawnRight = (pawns & ~FileHBB) >> 7;
        }
        ctx.attackedBy[c][Pawn] = pawnLeft | pawnRight;
        two |= pawnLeft & pawnRight;
        all = ctx.attackedBy[c][Pawn];

        Bitboard kingBB = board.byPiece[King] & board.byColor[c];
        if (kingBB) {
            Bitboard ka = KingAttacks[lsb(kingBB)];
            ctx.attackedBy[c][King] = ka;
            two |= all & ka;
            all |= ka;
        }

        Bitboard pieces = board.byPiece[Knight] & board.byColor[c];
        while (pieces) {
            Bitboard a = KnightAttacks[popLsb(pieces)];
            ctx.attackedBy[c][Knight] |= a;
            two |= all & a;
            all |= a;
        }

        pieces = board.byPiece[Bishop] & board.byColor[c];
        while (pieces) {
            Bitboard a = bishopAttacks(popLsb(pieces), occ);
            ctx.attackedBy[c][Bishop] |= a;
            two |= all & a;
            all |= a;
        }

        pieces = board.byPiece[Rook] & board.byColor[c];
        while (pieces) {
            Bitboard a = rookAttacks(popLsb(pieces), occ);
            ctx.attackedBy[c][Rook] |= a;
            two |= all & a;
            all |= a;
        }

        pieces = board.byPiece[Queen] & board.byColor[c];
        while (pieces) {
            Bitboard a = queenAttacks(popLsb(pieces), occ);
            ctx.attackedBy[c][Queen] |= a;
            two |= all & a;
            all |= a;
        }

        ctx.allAttacks[c] = all;
        ctx.attackedBy2[c] = two;
    }
}

// Thread-local pawn and material hashes so the multi-threaded tuner
// loss loop never races on shared writes. Real search uses one thread
// and behaves identically; each tuner worker gets its own table on
// first eval call inside that thread, freed when the thread exits.
// Tuner loss evals create fresh worker threads per call, so stale
// entries from a prior parameter snapshot never leak into the current
// loss eval.
static thread_local PawnHashTable pawnHashTable(2);
static thread_local MaterialHashTable materialHashTable(1);

// Compute the pure material contribution for this position: piece values
// (MG/EG), bishop pair bonus, quadratic imbalance, and game phase. The
// result depends only on piece counts, so it can be cached by the material
// zobrist key. PSTs are not included here because they depend on squares.
static void evaluateMaterial(const Board &board, Score outScores[2], int &outPhase) {
    int probeMg = 0, probeEg = 0, probePhase = 0;
    if (materialHashTable.probe(board.materialKey, probeMg, probeEg, probePhase)) {
        // Material hash stores a single white-minus-black packed score and
        // the game phase; assign it to White's slot and leave Black at 0.
        outScores[White] = S(probeMg, probeEg);
        outScores[Black] = 0;
        outPhase = probePhase;
        return;
    }

    int pc[2][6];
    int phase = 0;
    Score scores[2] = {0, 0};
    for (int c = 0; c < 2; c++) {
        for (int pt = 1; pt < 7; pt++) {
            int cnt = board.pieceCount[c][pt];
            scores[c] += evalParams.PieceScore[pt] * cnt;
            phase += GamePhaseInc[pt] * cnt;
        }
        pc[c][0] = (board.pieceCount[c][Bishop] >= 2) ? 1 : 0;
        pc[c][1] = board.pieceCount[c][Pawn];
        pc[c][2] = board.pieceCount[c][Knight];
        pc[c][3] = board.pieceCount[c][Bishop];
        pc[c][4] = board.pieceCount[c][Rook];
        pc[c][5] = board.pieceCount[c][Queen];
        if (pc[c][0]) scores[c] += evalParams.BishopPair;
    }
    int imbW = imbalance(pc, White);
    int imbB = imbalance(pc, Black);
    scores[White] += S(imbW, imbW);
    scores[Black] += S(imbB, imbB);

    Score delta = scores[White] - scores[Black];
    materialHashTable.store(board.materialKey, mg_value(delta), eg_value(delta), phase);
    outScores[White] = delta;
    outScores[Black] = 0;
    outPhase = phase;
}

// Per-side pawn structure evaluation without touching the hash. `perSide[c]`
// carries the unsigned contribution from color `c`; the combined
// white-minus-black value is left to the caller. Passer bitboards are
// populated as a side effect. Split out from `evaluatePawns` so the
// verbose breakdown can print each color's contribution separately
// without fighting the cache.
static void computePawns(const Board &board, Score perSide[2], Bitboard passers[2]) {
    Score sideScores[2] = {0, 0};
    Bitboard whitePassers = 0, blackPassers = 0;

    Bitboard whitePawns = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawns = board.byPiece[Pawn] & board.byColor[Black];

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = (c == White) ? whitePawns : blackPawns;
        Bitboard theirPawns = (c == White) ? blackPawns : whitePawns;
        Score &score = sideScores[c];

        // Pawn islands: project our pawns down to an 8-bit file mask
        // (one bit per file that contains at least one friendly pawn),
        // then count runs of set bits. Each run is an island; the
        // penalty fires once per extra island beyond the first since
        // one contiguous chain is the ideal structure. Cheap to fold
        // into the pawn hash because the computation depends only on
        // which files have pawns, which is a strict subset of the
        // information already keyed by pawnKey.
        Bitboard folded = ourPawns;
        folded |= folded >> 32;
        folded |= folded >> 16;
        folded |= folded >> 8;
        uint8_t fileBits = static_cast<uint8_t>(folded & 0xFFu);
        int islandCount = popcount(static_cast<Bitboard>(fileBits & ~(fileBits >> 1)));
        if (islandCount > 1) {
            score += evalParams.PawnIslandPenalty * (islandCount - 1);
        }

        Bitboard pawns = ourPawns;
        while (pawns) {
            int sq = popLsb(pawns);
            int r = squareRank(sq);
            int f = squareFile(sq);
            int relRank = (c == White) ? r : (7 - r);

            // Doubled pawn: another friendly pawn ahead on the same file
            bool isDoubled = (ForwardFileBB[c][sq] & ourPawns) != 0;
            if (isDoubled) {
                score += evalParams.DoubledPawnPenalty;
            }

            // "Unopposed" is the absence of any enemy pawn on the same
            // file ahead of this pawn. It is used by the weak-pawn term
            // below, and potentially by future pawn-structure signals, so
            // it is computed once here and reused.
            bool opposed = (ForwardFileBB[c][sq] & theirPawns) != 0;

            // Passed pawn: no enemy pawns ahead on same or adjacent files,
            // and no friendly pawn ahead on the same file (rear doubled pawns
            // are not passed)
            bool isPassed = !isDoubled && !(PassedPawnMask[c][sq] & theirPawns);
            if (isPassed) {
                score += evalParams.PassedPawnBonus[relRank];
                if (c == White)
                    whitePassers |= squareBB(sq);
                else
                    blackPassers |= squareBB(sq);
            }

            // Isolated pawn: no friendly pawns on adjacent files
            bool isolated = !(AdjacentFilesBB[f] & ourPawns);
            if (isolated) {
                score += evalParams.IsolatedPawnPenalty;
                // An isolated pawn with an open file behind it is an
                // easy rook target, so it absorbs an extra penalty on
                // top of the plain isolated cost. Skip when the pawn is
                // already passed: "no enemy pawn on the file ahead" is
                // exactly the feature the passed bonus rewards, so
                // double-counting it as a penalty here fights the passer
                // reward and mis-scores winning king-and-pawn endings.
                if (!opposed && !isPassed) {
                    score += evalParams.WeakUnopposedPenalty;
                }
                // Doubled and isolated together is the worst structural
                // configuration a pawn can sit in -- no rank neighbour,
                // no file neighbour that can defend it, and the rear
                // pawn inherits every weakness when the leader falls.
                if (isDoubled) {
                    score += evalParams.DoubledIsolatedPenalty;
                }
            }

            // Connected pawn: phalanx (same rank, adjacent file) or defended by friendly pawn
            bool phalanx = (ourPawns & AdjacentFilesBB[f] & RankBB[r]) != 0;
            bool defended = (PawnAttacks[c ^ 1][sq] & ourPawns) != 0;
            if (phalanx || defended) {
                score += evalParams.ConnectedPawnBonus[relRank];
                // A phalanx is the more dynamic half of the connected
                // family: the two pawns can advance together and cover
                // each other's stop squares on the next rank. Layering
                // a separate PhalanxBonus on top of the already-tuned
                // ConnectedPawnBonus overrewards phalanx without a
                // re-tune, so the term is disabled pending that work.
                // if (phalanx) {
                //     score +=evalParams.PhalanxBonus;
                // }
            }

            // Backward pawn: not connected, not isolated, all adjacent friendly pawns
            // are ahead, and the stop square is controlled by an enemy pawn
            if (!phalanx && !defended && !isolated) {
                bool noneBelow = !(PawnSpanMask[c ^ 1][sq] & ourPawns);
                if (noneBelow) {
                    int stopSq = (c == White) ? sq + 8 : sq - 8;
                    if (PawnAttacks[c][stopSq] & theirPawns) {
                        score += evalParams.BackwardPawnPenalty;
                        // Same "open file behind a weak pawn" logic as the
                        // isolated case: a backward pawn with no opposing
                        // pawn on its file is the easiest heavy-piece target
                        // of all.
                        if (!opposed) {
                            score += evalParams.WeakUnopposedPenalty;
                        }
                    }
                }
            }
        }
    }

    perSide[White] = sideScores[White];
    perSide[Black] = sideScores[Black];
    passers[White] = whitePassers;
    passers[Black] = blackPassers;
}

// Hashed per-side pawn evaluation. Reads from the pawn hash when the key
// matches (fast path; the cache does not preserve the per-side split, so
// `perSide` comes back as zeros). On a miss, `computePawns` runs the full
// calculation, the combined score is cached, and the per-side values are
// propagated to the caller.
static void evaluatePawns(const Board &board, Score &out, Bitboard passers[2], Score perSide[2]) {
    int mgCached = 0, egCached = 0;
    Bitboard cachedWhitePassers = 0, cachedBlackPassers = 0;
    if (pawnHashTable.probe(board.pawnKey, mgCached, egCached, cachedWhitePassers,
                            cachedBlackPassers)) {
        out = S(mgCached, egCached);
        passers[White] = cachedWhitePassers;
        passers[Black] = cachedBlackPassers;
        if (perSide) {
            perSide[White] = 0;
            perSide[Black] = 0;
        }
        return;
    }

    Score sides[2] = {0, 0};
    computePawns(board, sides, passers);
    Score combined = sides[White] - sides[Black];
    pawnHashTable.store(board.pawnKey, mg_value(combined), eg_value(combined), passers[White],
                        passers[Black]);
    out = combined;
    if (perSide) {
        perSide[White] = sides[White];
        perSide[Black] = sides[Black];
    }
}

// Accumulate piece-activity terms: mobility for every non-pawn non-king
// piece, rook bonuses for open and semi-open files, and outpost bonuses
// for knights and bishops. Mobility is intentionally pseudo-legal --
// pinned pieces still get credit for the squares they attack because the
// search resolves pin tactics on its own, which matches Stockfish's
// choice here.
static void evaluatePieces(const Board &board, const EvalContext &ctx, Score scores[2]) {
    Bitboard occ = board.occupied;

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[c];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[c ^ 1];

        Bitboard ourKingBB = board.byPiece[King] & board.byColor[c];
        int ourKingSq = ourKingBB ? lsb(ourKingBB) : -1;

        Bitboard theirKingOnly = board.byPiece[King] & board.byColor[c ^ 1];
        Bitboard theirKingRing =
            theirKingOnly ? kingZoneBB(lsb(theirKingOnly), static_cast<Color>(c ^ 1)) : 0;

        // Closed center signal: own pawns on the d or e file whose stop
        // square is occupied by any piece. Used to scale the per same
        // color pawn bishop penalty so a bad bishop trapped behind its
        // own central pawns hurts more than the same configuration on
        // an open board.
        Bitboard centerFiles = FileBB[3] | FileBB[4];
        Bitboard centerOurPawns = ourPawns & centerFiles;
        Bitboard centerStops = (c == White) ? (centerOurPawns << 8) : (centerOurPawns >> 8);
        int blockedCenterCount = popcount(centerStops & occ);

        // Minor behind pawn: a friendly knight or bishop sitting directly
        // one rank behind a friendly pawn is shielded against frontal
        // attacks and cannot be easily chased by an enemy pawn on the
        // same file. Pulling the pawn bitboard back one rank aligns the
        // pawns with their shielded minor, so a single AND counts every
        // shielded pair cleanly.
        Bitboard ourMinors = (board.byPiece[Knight] | board.byPiece[Bishop]) & board.byColor[c];
        Bitboard pawnsPulledBack = (c == White) ? (ourPawns >> 8) : (ourPawns << 8);
        int shieldedMinors = popcount(ourMinors & pawnsPulledBack);
        if (shieldedMinors) {
            scores[c] += evalParams.MinorBehindPawnBonus * shieldedMinors;
        }

        Bitboard knights = board.byPiece[Knight] & board.byColor[c];
        while (knights) {
            int sq = popLsb(knights);
            Bitboard atk = KnightAttacks[sq];
            int count = popcount(atk & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Knight][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += evalParams.KnightOutpostBonus;
            }

            if (theirKingRing && (atk & theirKingRing)) {
                scores[c] += evalParams.MinorOnKingRing;
            }

            if (ourKingSq >= 0) {
                scores[c] += evalParams.KingProtector * chebyshev(sq, ourKingSq);
            }
        }

        Bitboard bishops = board.byPiece[Bishop] & board.byColor[c];
        while (bishops) {
            int sq = popLsb(bishops);
            Bitboard atk = bishopAttacks(sq, occ);
            int count = popcount(atk & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Bishop][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += evalParams.BishopOutpostBonus;
            }

            Bitboard sameColorSquares =
                (squareBB(sq) & LightSquaresBB) ? LightSquaresBB : DarkSquaresBB;
            int sameColorPawnCount = popcount(ourPawns & sameColorSquares);
            if (sameColorPawnCount > 0) {
                scores[c] += evalParams.BadBishop;
                scores[c] += evalParams.BishopPawns * sameColorPawnCount * (1 + blockedCenterCount);
            }

            // X-ray pawns: enemy pawns the bishop sees through its own
            // pieces. Re-cast the bishop attack with own pieces removed
            // from the occupancy so the ray reaches every enemy pawn on
            // the diagonal, even past a friendly knight or pawn that
            // would be a blocker in the literal mobility sense.
            Bitboard xrayAttacks = bishopAttacks(sq, occ & ~board.byColor[c]);
            int xrayPawns = popcount(xrayAttacks & theirPawns);
            scores[c] += evalParams.BishopXrayPawns * xrayPawns;

            // Long diagonal sweep: the two central squares on this bishop's
            // long diagonal must both be covered by the bishop itself (from
            // the bishop's own square or through its attack set). If either
            // center square is blocked by a friendly or enemy piece, the
            // bonus stays off.
            Bitboard diag = 0;
            if (squareBB(sq) & DiagA1H8BB)
                diag = DiagA1H8BB;
            else if (squareBB(sq) & DiagA8H1BB)
                diag = DiagA8H1BB;
            if (diag) {
                Bitboard centerTwo = diag & (Rank4BB | Rank5BB);
                Bitboard reach = atk | squareBB(sq);
                if ((centerTwo & reach) == centerTwo) {
                    scores[c] += evalParams.BishopLongDiagonalBonus;
                }
            }

            if (theirKingRing && (atk & theirKingRing)) {
                scores[c] += evalParams.MinorOnKingRing;
            }

            if (ourKingSq >= 0) {
                scores[c] += evalParams.KingProtector * chebyshev(sq, ourKingSq);
            }
        }

        int kingFile = (ourKingSq >= 0) ? squareFile(ourKingSq) : -1;
        bool lostShortCastle = (c == White) ? !board.castleWK : !board.castleBK;
        bool lostLongCastle = (c == White) ? !board.castleWQ : !board.castleBQ;

        Bitboard enemyQueens = board.byPiece[Queen] & board.byColor[c ^ 1];
        Bitboard rooks = board.byPiece[Rook] & board.byColor[c];
        while (rooks) {
            int sq = popLsb(rooks);
            Bitboard rAtk = rookAttacks(sq, occ);
            int count = popcount(rAtk & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Rook][count];

            Bitboard fileMask = FileBB[squareFile(sq)];
            bool noOurPawns = !(fileMask & ourPawns);
            bool noTheirPawns = !(fileMask & theirPawns);
            if (noOurPawns && noTheirPawns) {
                scores[c] += evalParams.RookOpenFileBonus;
            } else if (noOurPawns) {
                scores[c] += evalParams.RookSemiOpenFileBonus;
            }

            // Rook on queen file: a small flat pressure bonus per
            // friendly rook sharing a file with an enemy queen.
            if (fileMask & enemyQueens) {
                scores[c] += evalParams.RookOnQueenFile;
            }

            // Rook on the seventh: either targets enemy pawns on the 7th
            // or pins the enemy king on the 8th. Both conditions generate
            // most of the classical "pig on the seventh" pressure.
            if (relativeRank(c, sq) == 6) {
                Bitboard seventhRankMask = (c == White) ? Rank7BB : Rank2BB;
                Bitboard eighthRankMask = (c == White) ? Rank8BB : Rank1BB;
                Bitboard theirKingBB = board.byPiece[King] & board.byColor[c ^ 1];
                if ((theirKingBB & eighthRankMask) || (theirPawns & seventhRankMask)) {
                    scores[c] += evalParams.RookOn7thBonus;
                }
            }

            // Trapped rook: little room to move and our king is on the same
            // side of the board, so the rook cannot swing across. Gating on
            // mobility rather than piece-square heuristics avoids the old
            // loophole where stepping the king off the back rank silenced
            // the penalty without actually freeing the rook.
            if (count <= 3 && ourKingSq >= 0) {
                int rookFile = squareFile(sq);
                bool sameSide = (kingFile < 4) == (rookFile < kingFile);
                if (sameSide) {
                    Score penalty = evalParams.TrappedRookByKingPenalty;
                    if (lostShortCastle && lostLongCastle) penalty *= 2;
                    scores[c] += penalty;
                }
            }

            // Rook on king ring fires only when the same rook also sits on
            // a (semi-)open file. The classical formulation tracks the
            // pressure that distinct lift-and-attack pattern carries, not
            // the generic multi-attacker count that the king-danger
            // accumulator already credits.
            if (noOurPawns && theirKingRing && (rAtk & theirKingRing)) {
                scores[c] += evalParams.RookOnKingRing;
            }
        }

        Bitboard queens = board.byPiece[Queen] & board.byColor[c];
        while (queens) {
            int sq = popLsb(queens);
            int count = popcount(queenAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Queen][count];
        }
    }
}

// Credit own pawns parked on the classical center squares. Primary
// (d/e file) and extended (c/f file) variants carry separate weights so
// a full c3-d4-e4 formation scores distinctly from a plain d4-e4 duo.
// Uses middlegame-only weights by design: in a deep endgame the file on
// which a pawn sits matters less than its passed/blocked status, which
// other terms already cover.
static void evaluateCentralPawns(const Board &board, Score scores[2]) {
    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[c];
        int primary = popcount(ourPawns & CentralDEFilesBB[c]);
        int extended = popcount(ourPawns & CentralCFFilesBB[c]);
        if (primary) scores[c] += evalParams.CentralPawnBonus[0] * primary;
        if (extended) scores[c] += evalParams.CentralPawnBonus[1] * extended;
    }
}

// Count central squares on our side of the board that are safe from enemy
// pawn attacks and not occupied by our own pawns. Squares behind our own
// pawn chain count double because they are already committed territory
// that amplifies the bonus. Scaled quadratically by the number of our
// non-pawn non-king pieces so the term only bites in middlegames with
// enough material to exploit the extra space.
static void evaluateSpace(const Board &board, const EvalContext &ctx, Score scores[2]) {
    for (int c = 0; c < 2; c++) {
        Bitboard ourPieces = board.byColor[c] & ~board.byPiece[Pawn] & ~board.byPiece[King];
        int weight = popcount(ourPieces);
        if (weight < SpaceMinPieceCount) continue;

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[c];
        Bitboard safe = SpaceMask[c] & ~ctx.pawnAttacks[c ^ 1] & ~ourPawns;

        Bitboard behind = ourPawns;
        if (c == White) {
            behind |= behind >> 8;
            behind |= behind >> 16;
        } else {
            behind |= behind << 8;
            behind |= behind << 16;
        }

        int bonus = popcount(safe) + popcount(safe & behind);
        scores[c] += S(bonus * weight * weight / SpaceWeightDivisor, 0);
    }
}

static void evaluateKingSafety(const Board &board, const EvalContext &ctx, Score scores[2]) {
    Bitboard occ = board.occupied;

    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);

        Bitboard kingBB = board.byPiece[King] & board.byColor[us];
        if (!kingBB) continue;
        int kingSq = lsb(kingBB);
        int kingFile = squareFile(kingSq);

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[them];

        // Classical shelter and storm: walk the three shield files
        // centered on the king (clamped so the window stays on the
        // board) and accumulate per file
        //   Shelter[edge_distance][our_rank]
        // and either
        //   BlockedStorm[their_rank]            if our pawn frontally
        //                                       blocks the rammer, or
        //   UnblockedStorm[edge_distance][their_rank]
        // otherwise. Rank 0 means "no pawn on file", which folds the
        // semi-open or open file penalty into the Shelter[d][0] entry
        // and contributes zero on the storm side.
        auto shelterStormAt = [&](int kf) -> Score {
            Score result = 0;
            int center = std::max(1, std::min(6, kf));
            for (int f = center - 1; f <= center + 1; f++) {
                int d = std::min(f, 7 - f);
                Bitboard fileMask = FileBB[f];
                Bitboard ourPawnsOnFile = ourPawns & fileMask;
                Bitboard theirPawnsOnFile = theirPawns & fileMask;
                int ourRank = 0;
                if (ourPawnsOnFile) {
                    int sq = (us == White) ? lsb(ourPawnsOnFile) : msb(ourPawnsOnFile);
                    int rr = (us == White) ? squareRank(sq) : (7 - squareRank(sq));
                    ourRank = std::min(rr, 6);
                }
                int theirRank = 0;
                if (theirPawnsOnFile) {
                    int sq = (us == White) ? lsb(theirPawnsOnFile) : msb(theirPawnsOnFile);
                    int rr = (us == White) ? squareRank(sq) : (7 - squareRank(sq));
                    theirRank = std::min(rr, 6);
                }
                result += evalParams.Shelter[d][ourRank];
                if (theirRank > 0) {
                    bool blocked = (ourRank > 0) && (ourRank == theirRank - 1);
                    if (blocked) {
                        result -= evalParams.BlockedStorm[theirRank];
                    } else {
                        result -= evalParams.UnblockedStorm[d][theirRank];
                    }
                }
            }
            return result;
        };

        bool canKingside = (us == White) ? board.castleWK : board.castleBK;
        bool canQueenside = (us == White) ? board.castleWQ : board.castleBQ;
        int castlingMask = (canKingside ? 0x1 : 0) | (canQueenside ? 0x2 : 0);

        // Cache the shelter and storm composite in the pawn hash. The
        // table cannot key on king position alone because the same
        // pawn structure with the king on a different file produces
        // a different score, so the entry stamps both the king file
        // and the castling mask. Cache misses on king or castling
        // changes; cache hits on the same pawn structure with no king
        // movement reuse the prior walk.
        Score shield;
        int cachedMg = 0;
        int cachedEg = 0;
        if (pawnHashTable.probeShelter(board.pawnKey, us, kingFile, castlingMask, cachedMg,
                                       cachedEg)) {
            shield = make_score(cachedMg, cachedEg);
        } else {
            shield = shelterStormAt(kingFile);
            // Best of current and castled candidate squares: the king
            // already in reach of a better shelter via castling should
            // not be statically penalized for sitting on the central
            // file pre-castle.
            if (canKingside) {
                Score alt = shelterStormAt(6);
                if (mg_value(alt) > mg_value(shield)) shield = alt;
            }
            if (canQueenside) {
                Score alt = shelterStormAt(2);
                if (mg_value(alt) > mg_value(shield)) shield = alt;
            }
            pawnHashTable.storeShelter(board.pawnKey, us, kingFile, castlingMask, mg_value(shield),
                                       eg_value(shield));
        }
        scores[us] += shield;

        // King zone attack evaluation. We accumulate both an integer
        // kingDanger score (fed to a quadratic mg / linear eg mapping) and
        // the per-attacker count the gate uses. All ingredients reuse the
        // shared attack maps in EvalContext so no new attack generation
        // runs here.
        Bitboard kZone = kingZoneBB(kingSq, us);
        int attackerCount = 0;
        int kingDangerMg = 0;
        int kingDangerEg = 0;

        Bitboard theirKnights = board.byPiece[Knight] & board.byColor[them];
        while (theirKnights) {
            if (KnightAttacks[popLsb(theirKnights)] & kZone) {
                attackerCount++;
                kingDangerMg += mg_value(evalParams.KingAttackByKnight);
                kingDangerEg += eg_value(evalParams.KingAttackByKnight);
            }
        }
        Bitboard theirBishops = board.byPiece[Bishop] & board.byColor[them];
        while (theirBishops) {
            if (bishopAttacks(popLsb(theirBishops), occ) & kZone) {
                attackerCount++;
                kingDangerMg += mg_value(evalParams.KingAttackByBishop);
                kingDangerEg += eg_value(evalParams.KingAttackByBishop);
            }
        }
        Bitboard theirRooks = board.byPiece[Rook] & board.byColor[them];
        while (theirRooks) {
            if (rookAttacks(popLsb(theirRooks), occ) & kZone) {
                attackerCount++;
                kingDangerMg += mg_value(evalParams.KingAttackByRook);
                kingDangerEg += eg_value(evalParams.KingAttackByRook);
            }
        }
        Bitboard theirQueens = board.byPiece[Queen] & board.byColor[them];
        while (theirQueens) {
            if (queenAttacks(popLsb(theirQueens), occ) & kZone) {
                attackerCount++;
                kingDangerMg += mg_value(evalParams.KingAttackByQueen);
                kingDangerEg += eg_value(evalParams.KingAttackByQueen);
            }
        }

        Bitboard enemyAttacks = ctx.allAttacks[them];
        Bitboard friendlyDefense = ctx.allAttacks[us];

        // Undefended zone squares: keep the existing Texel-tuned linear
        // term as-is, and additionally feed the same popcount into the
        // king-danger accumulator via KingRingWeakWeight so the quadratic
        // sees ring pressure too.
        Bitboard undefAttacked = kZone & enemyAttacks & ~friendlyDefense;
        int undefCount = popcount(undefAttacked);
        scores[us] += undefCount * evalParams.UndefendedKingZoneSq;
        kingDangerMg += undefCount * mg_value(evalParams.KingRingWeakWeight);
        kingDangerEg += undefCount * eg_value(evalParams.KingRingWeakWeight);

        // Safe checks: squares from which an enemy piece of each type
        // would give check to our king. A check square is "safe" when it
        // is reachable by the enemy piece, not occupied by another enemy
        // piece, and not defended by any of our non-king pieces. Squares
        // defended only by the king itself are treated as weak because a
        // supported attacker wins the capture: this is the classical
        // king-danger definition of safe check.
        Bitboard theirOcc = board.byColor[them];
        Bitboard nonKingDefense = ctx.attackedBy[us][Pawn] | ctx.attackedBy[us][Knight] |
                                  ctx.attackedBy[us][Bishop] | ctx.attackedBy[us][Rook] |
                                  ctx.attackedBy[us][Queen];
        Bitboard safeSquares = ~nonKingDefense & ~theirOcc;
        Bitboard knightCheckRays = KnightAttacks[kingSq];
        Bitboard bishopCheckRays = bishopAttacks(kingSq, occ);
        Bitboard rookCheckRays = rookAttacks(kingSq, occ);
        Bitboard queenCheckRays = bishopCheckRays | rookCheckRays;

        Bitboard safeKnightChecks = ctx.attackedBy[them][Knight] & knightCheckRays & safeSquares;
        Bitboard safeBishopChecks = ctx.attackedBy[them][Bishop] & bishopCheckRays & safeSquares;
        Bitboard safeRookChecks = ctx.attackedBy[them][Rook] & rookCheckRays & safeSquares;
        Bitboard safeQueenChecks = ctx.attackedBy[them][Queen] & queenCheckRays & safeSquares;

        kingDangerMg += popcount(safeKnightChecks) * mg_value(evalParams.KingSafeCheck[Knight]);
        kingDangerEg += popcount(safeKnightChecks) * eg_value(evalParams.KingSafeCheck[Knight]);
        kingDangerMg += popcount(safeBishopChecks) * mg_value(evalParams.KingSafeCheck[Bishop]);
        kingDangerEg += popcount(safeBishopChecks) * eg_value(evalParams.KingSafeCheck[Bishop]);
        kingDangerMg += popcount(safeRookChecks) * mg_value(evalParams.KingSafeCheck[Rook]);
        kingDangerEg += popcount(safeRookChecks) * eg_value(evalParams.KingSafeCheck[Rook]);
        kingDangerMg += popcount(safeQueenChecks) * mg_value(evalParams.KingSafeCheck[Queen]);
        kingDangerEg += popcount(safeQueenChecks) * eg_value(evalParams.KingSafeCheck[Queen]);

        // No-queen discount: the attack loses most of its bite when the
        // attacking side has no queen left on the board.
        if (!(board.byPiece[Queen] & board.byColor[them])) {
            kingDangerMg -= mg_value(evalParams.KingNoQueenDiscount);
            kingDangerEg -= eg_value(evalParams.KingNoQueenDiscount);
        }

        // King mobility differential: every safe square the king can
        // step to (not occupied by us, not attacked by them) reduces
        // the king-danger accumulator by KingMobilityFactor on each
        // half. Folded in before the clamp so a boxed-in king pays
        // the full quadratic while a king with escape squares pays
        // only the residual.
        Bitboard kingMoves = KingAttacks[kingSq] & ~board.byColor[us];
        int safeKingMoves = popcount(kingMoves & ~enemyAttacks);
        kingDangerMg -= safeKingMoves * mg_value(evalParams.KingMobilityFactor);
        kingDangerEg -= safeKingMoves * eg_value(evalParams.KingMobilityFactor);

        // Only penalize when at least 2 pieces attack the zone: a single
        // piece rarely creates a real mating threat on its own. Clamp
        // kingDanger to a non-negative bounded range before feeding the
        // quadratic so open-file positions with many safe-check squares
        // cannot compound into implausible penalties.
        if (attackerCount >= 2) {
            if (kingDangerMg < 0) kingDangerMg = 0;
            if (kingDangerEg < 0) kingDangerEg = 0;
            if (kingDangerMg > KingDangerMgCap) kingDangerMg = KingDangerMgCap;
            if (kingDangerEg > KingDangerEgCap) kingDangerEg = KingDangerEgCap;
            int mgPen = kingDangerMg * kingDangerMg / KingDangerDivMg;
            int egPen = kingDangerEg / KingDangerDivEg;
            scores[us] -= S(mgPen, egPen);
        }

        // Pawnless flank: when every file on our king's half of the
        // board (a..d if king is on the queenside, e..h otherwise) is
        // pawnless, the king sits in open territory the shelter / storm
        // grids cannot represent. Fold the penalty in unconditionally;
        // the magnitude already accounts for how often this shape
        // appears with the king still in the middlegame.
        Bitboard ourFlank = (kingFile < 4) ? (FileABB | FileBBB | FileCBB | FileDBB)
                                           : (FileEBB | FileFBB | FileGBB | FileHBB);
        if (!(board.byPiece[Pawn] & ourFlank)) {
            scores[us] += evalParams.PawnlessFlank;
        }

        // Endgame king to pawn distance: penalty per square of
        // Chebyshev distance from our king to our nearest pawn.
        // Captures the K+P endgame fundamental that the king must
        // walk toward its pawns to support them, without limiting the
        // signal to passers the way PassedKingProxBonus does. Eg-only
        // by construction; the mg half stays zero in the table.
        Bitboard ourPawnsKpd = board.byPiece[Pawn] & board.byColor[us];
        if (ourPawnsKpd) {
            int minDist = 8;
            Bitboard iter = ourPawnsKpd;
            while (iter) {
                int psq = popLsb(iter);
                int d = chebyshev(kingSq, psq);
                if (d < minDist) minDist = d;
            }
            scores[us] += S(0, eg_value(evalParams.KingPawnDistEg) * minDist);
        }
    }
}

// Apply king-distance, blockade, support, and connected-passer bonuses
// on top of the rank-based evalParams.PassedPawnBonus. Iterates the cached passer
// bitboards (which is why these terms live outside the pawn hash:
// they depend on king and piece positions that are not part of the pawn
// structure). Only passers on relative rank 4 or higher participate,
// matching the entries in the bonus tables.
static void evaluatePassedPawnExtras(const Board &board, const EvalContext &ctx,
                                     const Bitboard passers[2], Score scores[2]) {
    Bitboard occ = board.occupied;

    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);

        Bitboard ourPassers = passers[us];
        Bitboard theirPassers = passers[them];
        if (!ourPassers && !theirPassers) continue;

        Bitboard ourKingBB = board.byPiece[King] & board.byColor[us];
        Bitboard theirKingBB = board.byPiece[King] & board.byColor[them];
        int ourKingSq = ourKingBB ? lsb(ourKingBB) : -1;
        int theirKingSq = theirKingBB ? lsb(theirKingBB) : -1;

        Bitboard remaining = ourPassers;
        while (remaining) {
            int sq = popLsb(remaining);
            int relRank = relativeRank(us, sq);
            if (relRank < 3) continue;

            int stopSq = (us == White) ? sq + 8 : sq - 8;
            Bitboard stopBB = squareBB(stopSq);

            // Our king close to the stop square helps escort the passer, so
            // the bonus grows as chebyshev distance shrinks. Enemy king
            // close threatens to blockade, so the penalty grows the same
            // way. Use (7 - distance) so the tables read as "per step of
            // closeness" rather than "per step of remoteness".
            if (ourKingSq >= 0) {
                int closeness = 7 - chebyshev(ourKingSq, stopSq);
                scores[us] += evalParams.PassedKingProxBonus[relRank] * closeness;
            }
            if (theirKingSq >= 0) {
                int closeness = 7 - chebyshev(theirKingSq, stopSq);
                scores[us] -= evalParams.PassedEnemyKingProxPenalty[relRank] * closeness;
            }

            bool stopEmpty = !(occ & stopBB);
            if (!stopEmpty && (board.byColor[them] & stopBB)) {
                scores[us] += evalParams.PassedBlockedPenalty[relRank];
            } else if (stopEmpty && (ctx.allAttacks[us] & stopBB) &&
                       !(ctx.allAttacks[them] & stopBB)) {
                // Only a truly safe stop square counts as supported -- if
                // the enemy attacks it too, the push loses the pawn.
                scores[us] += evalParams.PassedSupportedBonus[relRank];
            }
        }

        // Tarrasch's rule: rooks are strongest behind passed pawns. A rook
        // on the same file as a friendly passer, positioned on the passer's
        // rear, escorts it toward promotion while shadowing any blockader;
        // the same rook planted behind an enemy passer chases it down from
        // the rear and ties the defender to its pawn. "Behind our passer"
        // uses the enemy's forward-file mask (squares below the passer
        // from our side); "behind their passer" uses our own forward-file
        // mask (squares above the enemy passer).
        Bitboard ourRooks = board.byPiece[Rook] & board.byColor[us];
        if (ourRooks) {
            Bitboard ownPasserIter = ourPassers;
            while (ownPasserIter) {
                int sq = popLsb(ownPasserIter);
                int behind = popcount(ourRooks & ForwardFileBB[them][sq]);
                if (behind) {
                    scores[us] += evalParams.RookBehindOurPasserBonus * behind;
                }
            }
            Bitboard enemyPasserIter = theirPassers;
            while (enemyPasserIter) {
                int sq = popLsb(enemyPasserIter);
                int behind = popcount(ourRooks & ForwardFileBB[us][sq]);
                if (behind) {
                    scores[us] += evalParams.RookBehindTheirPasserBonus * behind;
                }
            }
        }

        // Connected passers: pair up passers on adjacent files and within
        // one rank of each other. Iterate over every passer (even below
        // the bonus-eligible rank) so pairs where one side is just under
        // the threshold are still credited against the higher-ranked
        // member. Looking only at file+1 keeps each pair counted once.
        Bitboard pairIter = ourPassers;
        while (pairIter) {
            int sq = popLsb(pairIter);
            int f = squareFile(sq);
            int r = squareRank(sq);
            if (f >= 7) continue;

            Bitboard neighborMask = FileBB[f + 1];
            Bitboard partners = ourPassers & neighborMask;
            for (int rr = std::max(0, r - 1); rr <= std::min(7, r + 1); rr++) {
                if (Bitboard hit = partners & RankBB[rr]) {
                    int partnerSq = lsb(hit);
                    int higherRelRank = std::max(relativeRank(us, sq), relativeRank(us, partnerSq));
                    if (higherRelRank >= 3) {
                        scores[us] += evalParams.ConnectedPassersBonus[higherRelRank];
                    }
                    break;
                }
            }
        }
    }
}

// Penalize non-passer pawns whose stop square is occupied by an enemy
// piece and which sit on relative rank 5 or 6. Passers already receive
// PassedBlockedPenalty via evaluatePassedPawnExtras, so this term skips
// them and only reports on "stuck" pawns that never had the passer
// upside to begin with. Lives outside the pawn hash because blocker
// identity depends on non-pawn pieces.
static void evaluateBlockedPawns(const Board &board, const Bitboard passers[2], Score scores[2]) {
    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard blockedCandidates = ourPawns & ~passers[us];

        while (blockedCandidates) {
            int sq = popLsb(blockedCandidates);
            int relRank = relativeRank(us, sq);
            if (relRank < 4 || relRank > 5) continue;

            int stopSq = (us == White) ? sq + 8 : sq - 8;
            if (board.byColor[us ^ 1] & squareBB(stopSq)) {
                scores[us] += evalParams.BlockedPawnPenalty[relRank - 4];
            }
        }
    }
}

// Compute a position-wide initiative contribution. The magnitude is
// accumulated from six features (passer count, pawn count, outflank,
// pawn tension, king infiltration, pure-pawn-endgame flag) plus a
// constant baseline, then signed by the side with the current eg-level
// positional advantage. A clamp prevents the contribution from flipping
// the sign of an already small eg score. The mg half is applied at half
// strength so the term does not distort quiet middlegame positions.
static Score evaluateInitiative(const Board &board, const EvalContext &ctx,
                                const Bitboard passers[2], Score totalBeforeInitiative) {
    // Initiative is a positional-complexity signal. It only makes sense
    // when both sides still have pawns to work with: pawnless endings
    // are resolved by material fundamentals, not by positional tension.
    Bitboard whitePawnsBB = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawnsBB = board.byPiece[Pawn] & board.byColor[Black];
    if (!whitePawnsBB || !blackPawnsBB) return S(0, 0);

    int passerCount = popcount(passers[White] | passers[Black]);
    int pawnCount = popcount(board.byPiece[Pawn]);

    Bitboard allPawns = board.byPiece[Pawn];
    int kingsidePawns = popcount(allPawns & KingSideBB);
    int queensidePawns = popcount(allPawns & QueenSideBB);
    int outflank = kingsidePawns * queensidePawns;
    if (outflank > 16) outflank = 16;

    // Tension is counted from both sides so a symmetric break is
    // credited twice. The weight (4 eg) already accounts for that
    // double counting at a scale matched to other initiative weights.
    int tension = popcount(ctx.pawnAttacks[White] & blackPawnsBB) +
                  popcount(ctx.pawnAttacks[Black] & whitePawnsBB);

    int infiltrated = 0;
    Bitboard whiteKingBB = board.byPiece[King] & board.byColor[White];
    Bitboard blackKingBB = board.byPiece[King] & board.byColor[Black];
    if (whiteKingBB && squareRank(lsb(whiteKingBB)) >= 4) infiltrated++;
    if (blackKingBB && squareRank(lsb(blackKingBB)) <= 3) infiltrated++;

    bool onlyPawns = (board.byPiece[Pawn] != 0) &&
                     (board.pieceCount[White][Knight] + board.pieceCount[White][Bishop] +
                          board.pieceCount[White][Rook] + board.pieceCount[White][Queen] +
                          board.pieceCount[Black][Knight] + board.pieceCount[Black][Bishop] +
                          board.pieceCount[Black][Rook] + board.pieceCount[Black][Queen] ==
                      0);

    int mgMag = mg_value(evalParams.InitiativePasser) * passerCount +
                mg_value(evalParams.InitiativePawnCount) * pawnCount +
                mg_value(evalParams.InitiativeOutflank) * outflank +
                mg_value(evalParams.InitiativeTension) * tension +
                mg_value(evalParams.InitiativeInfiltrate) * infiltrated +
                mg_value(evalParams.InitiativePureBase) * (onlyPawns ? 1 : 0) +
                mg_value(evalParams.InitiativeConstant);
    int egMag = eg_value(evalParams.InitiativePasser) * passerCount +
                eg_value(evalParams.InitiativePawnCount) * pawnCount +
                eg_value(evalParams.InitiativeOutflank) * outflank +
                eg_value(evalParams.InitiativeInfiltrate) * infiltrated +
                eg_value(evalParams.InitiativePureBase) * (onlyPawns ? 1 : 0) +
                eg_value(evalParams.InitiativeConstant);

    int egBefore = eg_value(totalBeforeInitiative);
    int sign = (egBefore > 0) - (egBefore < 0);
    if (sign == 0) return S(0, 0);

    // Linearly ramp the initiative contribution from zero at eg=0 up to
    // full magnitude at |eg| >= InitiativeRampScale (one pawn in
    // internal units). The naive discrete sign flip at eg=0 produced a
    // 2 * egMag swing for a 2-unit change in the underlying signal,
    // which destabilized alpha-beta aspiration windows and inflated the
    // node count from startpos at depth 10 by roughly 2.5x against the
    // pre-term baseline. The ramp keeps the endgame behavior intact
    // (full magnitude once the advantage is established) while making
    // the transition through eg=0 continuous.
    const int InitiativeRampScale = 228;
    int absEg = std::abs(egBefore);
    int ramp = (absEg >= InitiativeRampScale) ? InitiativeRampScale : absEg;
    int egDelta = sign * egMag * ramp / InitiativeRampScale;
    // Clamp so the contribution cannot flip the sign of an already
    // small eg score even when egMag is itself negative (typical when
    // the baseline constant dominates the feature sum).
    if (sign > 0 && egDelta < -egBefore) egDelta = -egBefore;
    if (sign < 0 && egDelta > -egBefore) egDelta = -egBefore;

    int mgDelta = sign * mgMag * ramp / (2 * InitiativeRampScale);

    return S(mgDelta, egDelta);
}

// Endgame scale factor in [0, 64]. Applied to the eg half of the tapered
// score before blending with mg: at 64 the eg value passes through
// unchanged, at 0 the eg contribution disappears entirely. Used to push
// drawish endings (opposite-colored bishops, pawnless single minors)
// toward zero so the engine does not chase illusory winning lines.
static int scaleFactor(const Board &board) {
    // Opposite-colored bishops: each side has exactly one bishop, on
    // squares of opposite colors. Without other pieces they are a textbook
    // draw; with rooks or minors they are still visibly drawish.
    bool ocb = false;
    if (board.pieceCount[White][Bishop] == 1 && board.pieceCount[Black][Bishop] == 1) {
        Bitboard whiteBishop = board.byPiece[Bishop] & board.byColor[White];
        Bitboard blackBishop = board.byPiece[Bishop] & board.byColor[Black];
        bool whiteOnLight = (whiteBishop & LightSquaresBB) != 0;
        bool blackOnLight = (blackBishop & LightSquaresBB) != 0;
        ocb = (whiteOnLight != blackOnLight);
    }

    if (ocb) {
        int nonBishopPieces = popcount(board.occupied) - popcount(board.byPiece[Pawn]) -
                              popcount(board.byPiece[King]) - popcount(board.byPiece[Bishop]);
        if (nonBishopPieces > 0) {
            // OCB alongside rooks, knights, or queens is not reliably
            // drawish, and scaling it down distorts middlegame positions
            // that transiently feature opposite-colored bishops. Leave
            // those untouched and only correct genuine bishops-and-pawns
            // OCB endings below.
            return 64;
        }
        int strongPawns = std::max(board.pieceCount[White][Pawn], board.pieceCount[Black][Pawn]);
        if (strongPawns <= 1) return 10;
        if (strongPawns <= 3) return 26;
        return 38;
    }

    return 64;
}

// Reward pieces we attack with a less-valuable attacker, hanging pieces,
// weak queens, and enemy pieces hit by a safe pawn push. Threat types are
// indexed by the victim piece so lower-value attackers never double-count
// against equally-valued victims. All terms consume the shared attack
// maps precomputed in buildAttackMaps.
static void evaluateThreats(const Board &board, const EvalContext &ctx, Score scores[2]) {
    const int escScale = eg_value(evalParams.EscapableThreatScale);
    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);

        Bitboard theirPieces = board.byColor[them];
        Bitboard theirNonPawnNonKing = theirPieces & ~board.byPiece[Pawn] & ~board.byPiece[King];

        // Pieces of `them` that have no quiet square to move to without
        // landing in the attack footprint of a strictly-lower-value
        // piece of ours. Computed per target type because the
        // "lower-value attackers" set differs (a queen avoids P/N/B/R,
        // a rook avoids P/N/B, a minor avoids P). Used below to split
        // each threat term into "stuck" victims (full credit) and
        // "escapable" victims (scaled down by EscapableThreatScale).
        // Quiet squares only - captures are qsearch's responsibility,
        // so we do not count "escape by capturing an undefended enemy"
        // here.
        Bitboard ourLowerForQueen = ctx.attackedBy[us][Pawn] | ctx.attackedBy[us][Knight] |
                                    ctx.attackedBy[us][Bishop] | ctx.attackedBy[us][Rook];
        Bitboard ourLowerForRook =
            ctx.attackedBy[us][Pawn] | ctx.attackedBy[us][Knight] | ctx.attackedBy[us][Bishop];
        Bitboard ourLowerForMinor = ctx.attackedBy[us][Pawn];
        Bitboard occ = board.occupied;
        auto stuckFor = [&](PieceType pt, Bitboard lowerAttacks) {
            Bitboard stuck = 0;
            Bitboard iter = theirPieces & board.byPiece[pt];
            while (iter) {
                int sq = popLsb(iter);
                Bitboard moves;
                if (pt == Knight)
                    moves = KnightAttacks[sq];
                else if (pt == Bishop)
                    moves = bishopAttacks(sq, occ);
                else if (pt == Rook)
                    moves = rookAttacks(sq, occ);
                else
                    moves = queenAttacks(sq, occ);
                Bitboard safeQuiet = moves & ~occ & ~lowerAttacks;
                if (!safeQuiet) stuck |= squareBB(sq);
            }
            return stuck;
        };
        Bitboard stuckQueens = stuckFor(Queen, ourLowerForQueen);
        Bitboard stuckRooks = stuckFor(Rook, ourLowerForRook);
        Bitboard stuckBishops = stuckFor(Bishop, ourLowerForMinor);
        Bitboard stuckKnights = stuckFor(Knight, ourLowerForMinor);

        // Combine "full credit for stuck victims" with "scaled credit
        // for escapable victims" into a single Score. Score is packed
        // (eg << 16) + mg, so division and large multiplications do
        // not commute with the packing; we unpack each half, apply
        // the per-half arithmetic, and repack via S(). The aggregate
        // multiplier (stuck * 64 + esc * escScale) divided by 64
        // produces the weighted total without an intermediate divide
        // on the packed Score.
        auto scaleByCounts = [&](const Score &full, int stuck, int esc) {
            int factor = stuck * 64 + esc * escScale;
            int mg = mg_value(full) * factor / 64;
            int eg = eg_value(full) * factor / 64;
            return S(mg, eg);
        };
        auto creditSplit = [&](const Score &full, Bitboard victims, Bitboard stuckSet) {
            int stuck = popcount(victims & stuckSet);
            int esc = popcount(victims & ~stuckSet);
            return scaleByCounts(full, stuck, esc);
        };

        // Threat by pawn: every enemy non-pawn/non-king attacked by one
        // of our pawns. Pawn attacks cannot be reciprocated by a lower-
        // value attacker, but the target can still side-step the pawn
        // by moving to a square our pawns don't cover - which is the
        // "stuck" test for that piece type.
        Bitboard pawnThreats = ctx.pawnAttacks[us] & theirNonPawnNonKing;
        scores[us] +=
            creditSplit(evalParams.ThreatByPawn, pawnThreats & board.byPiece[Queen], stuckQueens);
        scores[us] +=
            creditSplit(evalParams.ThreatByPawn, pawnThreats & board.byPiece[Rook], stuckRooks);
        scores[us] +=
            creditSplit(evalParams.ThreatByPawn, pawnThreats & board.byPiece[Bishop], stuckBishops);
        scores[us] +=
            creditSplit(evalParams.ThreatByPawn, pawnThreats & board.byPiece[Knight], stuckKnights);

        // Threat by minor: our knights or bishops attacking an enemy
        // rook or queen. Indexed by victim so same-value targets are
        // naturally zero. Each victim type takes the SEE-aware split
        // against the matching stuck-set.
        Bitboard minorAttacks = ctx.attackedBy[us][Knight] | ctx.attackedBy[us][Bishop];
        Bitboard victims = minorAttacks & theirPieces;
        Bitboard minorVsRook = victims & board.byPiece[Rook];
        Bitboard minorVsQueen = victims & board.byPiece[Queen];
        scores[us] += creditSplit(evalParams.ThreatByMinor[Rook], minorVsRook, stuckRooks);
        scores[us] += creditSplit(evalParams.ThreatByMinor[Queen], minorVsQueen, stuckQueens);

        // Threat by rook: our rooks attacking an enemy queen.
        Bitboard rookVsQueen = ctx.attackedBy[us][Rook] & theirPieces & board.byPiece[Queen];
        scores[us] += creditSplit(evalParams.ThreatByRook[Queen], rookVsQueen, stuckQueens);

        // Threat by king: enemy pieces sitting on a square our king attacks
        // and which the enemy does not defend. Pawns and kings are excluded
        // from the set of victims.
        Bitboard kingVictims =
            ctx.attackedBy[us][King] & theirNonPawnNonKing & ~ctx.allAttacks[them];
        scores[us] += evalParams.ThreatByKing * popcount(kingVictims);

        // Hanging pieces: the conventional weak-piece set is enemy
        // non-pawn / non-king pieces we attack that are either
        // undefended or under-defended (we attack with two or more
        // pieces while the defender does not match the count).
        // To avoid double crediting, we subtract any victim already
        // paid out by a less-valuable attacker via ThreatBy*: pieces
        // hit by our pawns (ThreatByPawn), rooks or queens hit by
        // our minors (ThreatByMinor), and queens hit by our rooks
        // (ThreatByRook). The remaining set is the "free" weak-piece
        // signal that no other term has credited.
        Bitboard weak = theirNonPawnNonKing & ctx.allAttacks[us] &
                        (~ctx.allAttacks[them] | (ctx.attackedBy2[us] & ~ctx.attackedBy2[them]));
        Bitboard rooksAndQueensTheirs = (board.byPiece[Rook] | board.byPiece[Queen]) & theirPieces;
        Bitboard queensTheirs = board.byPiece[Queen] & theirPieces;
        Bitboard creditedElsewhere =
            ctx.pawnAttacks[us] |
            ((ctx.attackedBy[us][Knight] | ctx.attackedBy[us][Bishop]) & rooksAndQueensTheirs) |
            (ctx.attackedBy[us][Rook] & queensTheirs);
        Bitboard hanging = weak & ~creditedElsewhere;
        scores[us] += evalParams.Hanging * popcount(hanging);

        // Weak queen: enemy queen attacked by two or more of our pieces.
        // Single queen positions are the common case so we split via the
        // bitboard itself rather than per-square counting.
        Bitboard weakQueens = theirPieces & board.byPiece[Queen] & ctx.attackedBy2[us];
        if (weakQueens) {
            int stuckCnt = popcount(weakQueens & stuckQueens);
            int escCnt = popcount(weakQueens & ~stuckQueens);
            scores[us] += scaleByCounts(evalParams.WeakQueen, stuckCnt, escCnt);
        }

        // Slider on queen: every friendly bishop or rook whose ray to an
        // enemy queen passes through exactly one intermediate piece. The
        // SEE-aware split runs per queen so x-rays on a stuck queen earn
        // full credit while x-rays on a queen with quiet retreats earn
        // only the scaled fraction.
        Bitboard enemyQueens = theirPieces & board.byPiece[Queen];
        Bitboard ourBishops = board.byPiece[Bishop] & board.byColor[us];
        Bitboard ourRooks = board.byPiece[Rook] & board.byColor[us];
        if (enemyQueens && (ourBishops | ourRooks)) {
            int diagXraysStuck = 0;
            int diagXraysEsc = 0;
            int orthoXraysStuck = 0;
            int orthoXraysEsc = 0;
            Bitboard queensIter = enemyQueens;
            while (queensIter) {
                int qSq = popLsb(queensIter);
                bool stuck = (squareBB(qSq) & stuckQueens) != 0;
                if (ourBishops) {
                    Bitboard firstDiag = bishopAttacks(qSq, occ) & occ;
                    Bitboard xraySquaresDiag = bishopAttacks(qSq, occ ^ firstDiag);
                    int xrays = popcount(xraySquaresDiag & ourBishops & ~firstDiag);
                    if (stuck)
                        diagXraysStuck += xrays;
                    else
                        diagXraysEsc += xrays;
                }
                if (ourRooks) {
                    Bitboard firstOrtho = rookAttacks(qSq, occ) & occ;
                    Bitboard xraySquaresOrtho = rookAttacks(qSq, occ ^ firstOrtho);
                    int xrays = popcount(xraySquaresOrtho & ourRooks & ~firstOrtho);
                    if (stuck)
                        orthoXraysStuck += xrays;
                    else
                        orthoXraysEsc += xrays;
                }
            }
            scores[us] +=
                scaleByCounts(evalParams.SliderOnQueenBishop, diagXraysStuck, diagXraysEsc);
            scores[us] +=
                scaleByCounts(evalParams.SliderOnQueenRook, orthoXraysStuck, orthoXraysEsc);
        }

        // Restricted piece: every square we attack that an enemy knight,
        // bishop, rook, or queen also attacks, minus squares their pawns
        // defend. Each such square limits one of their pieces from
        // retreating or rotating through, which is the positional
        // coordination signal the term models.
        Bitboard theirPieceAttacks = ctx.attackedBy[them][Knight] | ctx.attackedBy[them][Bishop] |
                                     ctx.attackedBy[them][Rook] | ctx.attackedBy[them][Queen];
        Bitboard restricted = theirPieceAttacks & ctx.allAttacks[us] & ~ctx.pawnAttacks[them];
        scores[us] += evalParams.RestrictedPiece * popcount(restricted);

        // Safe pawn push threat: single- or double-push targets that are
        // empty, not attacked by enemy pawns, and either not attacked by
        // the enemy at all or defended by our own pieces. From those
        // safe landing squares, compute the pawn attack footprint and
        // bonus every enemy non-pawn/non-king piece that falls under it.
        // Exclude pieces already attacked by our pawns in place so the
        // bonus is not double counted with evalParams.ThreatByPawn. The
        // SEE-aware split applies: a pushable threat against a queen
        // with quiet retreats is much weaker than against a stuck one.
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard empty = ~board.occupied;
        Bitboard singlePush = (us == White) ? (ourPawns << 8) : (ourPawns >> 8);
        singlePush &= empty;
        Bitboard doublePush =
            (us == White) ? ((singlePush & Rank3BB) << 8) : ((singlePush & Rank6BB) >> 8);
        doublePush &= empty;
        Bitboard pushes = singlePush | doublePush;
        Bitboard safePushes =
            pushes & ~ctx.pawnAttacks[them] & (~ctx.allAttacks[them] | ctx.allAttacks[us]);
        Bitboard pushVictims =
            pawnAttacksBB(safePushes, us) & theirNonPawnNonKing & ~ctx.pawnAttacks[us];
        scores[us] +=
            creditSplit(evalParams.SafePawnPush, pushVictims & board.byPiece[Queen], stuckQueens);
        scores[us] +=
            creditSplit(evalParams.SafePawnPush, pushVictims & board.byPiece[Rook], stuckRooks);
        scores[us] +=
            creditSplit(evalParams.SafePawnPush, pushVictims & board.byPiece[Bishop], stuckBishops);
        scores[us] +=
            creditSplit(evalParams.SafePawnPush, pushVictims & board.byPiece[Knight], stuckKnights);

        // Per-square pawn-push threat: walk every push target (safe or
        // not) and tally each (push square, victim piece) pair where the
        // push lands attacking an enemy non-pawn / non-king. Pushes the
        // opponent must address with a tempo even when capturing the
        // pawn is on the board, so the signal is distinct from
        // SafePawnPush. Already-pawn-attacked victims are filtered out
        // to keep the bonus orthogonal to ThreatByPawn. SEE-aware split
        // accumulates stuck and escapable victims separately and
        // applies the discount in one shot at the end.
        int pushStuck = 0;
        int pushEsc = 0;
        Bitboard pushIter = pushes;
        while (pushIter) {
            int psq = popLsb(pushIter);
            Bitboard pushVic = PawnAttacks[us][psq] & theirNonPawnNonKing & ~ctx.pawnAttacks[us];
            Bitboard stuckSet = (pushVic & board.byPiece[Queen] & stuckQueens) |
                                (pushVic & board.byPiece[Rook] & stuckRooks) |
                                (pushVic & board.byPiece[Bishop] & stuckBishops) |
                                (pushVic & board.byPiece[Knight] & stuckKnights);
            pushStuck += popcount(stuckSet);
            pushEsc += popcount(pushVic) - popcount(stuckSet);
        }
        scores[us] += scaleByCounts(evalParams.ThreatByPawnPush, pushStuck, pushEsc);

        // Weak-piece-protected-only-by-queen: any friendly non-pawn
        // minor or rook under enemy attack whose only defender is the
        // queen. The queen's defense is tempo-fragile because every
        // recapture trades down a major for a minor or pawn, so a
        // piece that only the queen defends carries a structurally
        // worse risk profile than one a less-valuable piece protects.
        // Pawns and the queen herself are excluded: pawns get queen-
        // defended in routine endgame patterns where the term would
        // just misfire, and a queen defending herself is not the
        // discovery / overload signal we want to capture.
        Bitboard ourMinorsAndRooks =
            board.byColor[us] &
            (board.byPiece[Knight] | board.byPiece[Bishop] | board.byPiece[Rook]);
        Bitboard underAttack = ourMinorsAndRooks & ctx.allAttacks[them];
        Bitboard nonQueenDefense = ctx.attackedBy[us][Pawn] | ctx.attackedBy[us][Knight] |
                                   ctx.attackedBy[us][Bishop] | ctx.attackedBy[us][Rook] |
                                   ctx.attackedBy[us][King];
        Bitboard onlyQueenDef = underAttack & ctx.attackedBy[us][Queen] & ~nonQueenDefense;
        scores[us] += evalParams.WeakQueenDefender * popcount(onlyQueenDef);

        // Knight on queen: each friendly knight that has two or more
        // safe candidate squares from which it would attack the enemy
        // queen earns a bonus. "Safe" means the candidate square is
        // not occupied by us and not attacked by any enemy piece
        // (queen attacks count as enemy attacks even though the queen
        // is the target -- if the queen is the only enemy defender,
        // recapturing the knight loses the queen too, but we cannot
        // tell that without a SEE walk, so the strict filter avoids
        // claiming forks the simple version cannot prove are real).
        // Iterate over every enemy queen so a promoted second queen
        // also feeds the fork-threat tally; a knight that
        // simultaneously forks two queens scores twice, which matches
        // the pressure both threats exert on the defender's tempo
        // budget.
        if (enemyQueens) {
            Bitboard ourKnightsBase = board.byPiece[Knight] & board.byColor[us];
            Bitboard safeForKnight = ~board.byColor[us] & ~ctx.allAttacks[them];
            int knightForks = 0;
            Bitboard queensIter = enemyQueens;
            while (queensIter) {
                int queenSq = popLsb(queensIter);
                Bitboard forkSquares = KnightAttacks[queenSq];
                Bitboard knightsIter = ourKnightsBase;
                while (knightsIter) {
                    int nsq = popLsb(knightsIter);
                    Bitboard candidates = KnightAttacks[nsq] & forkSquares & safeForKnight;
                    if (popcount(candidates) >= 2) knightForks++;
                }
            }
            scores[us] += evalParams.KnightOnQueen * knightForks;
        }

        // Queen infiltration: our queen sits on the enemy half of the
        // board on a square neither an enemy pawn nor an enemy minor
        // attacks. Such a queen cannot be cheaply evicted and exerts
        // sustained pressure across files and ranks: it picks up any
        // weak target the rest of the eval flags without taking the
        // recapture-risk hit a queen normally pays for foraying past
        // the midline.
        Bitboard ourQueensInf = board.byPiece[Queen] & board.byColor[us];
        if (ourQueensInf) {
            Bitboard enemyHalf = (us == White) ? (Rank5BB | Rank6BB | Rank7BB | Rank8BB)
                                               : (Rank1BB | Rank2BB | Rank3BB | Rank4BB);
            Bitboard safeForQueen = ~ctx.pawnAttacks[them] & ~ctx.attackedBy[them][Knight] &
                                    ~ctx.attackedBy[them][Bishop];
            int infiltrated = popcount(ourQueensInf & enemyHalf & safeForQueen);
            scores[us] += evalParams.QueenInfiltration * infiltrated;
        }
    }
}

int evaluate(const Board &board) {
    ensureEvalInit();

    Score scores[2] = {0, 0};
    int gamePhase = 0;

    // PST-only accumulation per square; piece material, bishop pair, and
    // imbalance come from the cached material probe below. PawnPST is
    // full-board (asymmetric structure), non-pawn PSTs are half-board
    // (file mirrored to queenside) so a knight on c3 and a knight on
    // f3 read the same tunable.
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx;
        if (p.type == Pawn) {
            idx = (p.color == White) ? sq : (sq ^ 56);
        } else {
            int rrank = (p.color == White) ? squareRank(sq) : (7 - squareRank(sq));
            int file = squareFile(sq);
            int fIdx = std::min(file, 7 - file);
            idx = (rrank << 2) | fIdx;
        }
        scores[p.color] += PST[p.type][idx];
    }

    Score matScores[2];
    evaluateMaterial(board, matScores, gamePhase);
    scores[White] += matScores[White];
    scores[Black] += matScores[Black];

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;

    Score pawnScore = 0;
    Bitboard passers[2] = {0, 0};
    evaluatePawns(board, pawnScore, passers, nullptr);

    EvalContext ctx;
    Bitboard whitePawnsCtx = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawnsCtx = board.byPiece[Pawn] & board.byColor[Black];
    ctx.pawnAttacks[White] = pawnAttacksBB(whitePawnsCtx, White);
    ctx.pawnAttacks[Black] = pawnAttacksBB(blackPawnsCtx, Black);
    ctx.mobilityArea[White] = ~board.byColor[White] & ~ctx.pawnAttacks[Black];
    ctx.mobilityArea[Black] = ~board.byColor[Black] & ~ctx.pawnAttacks[White];
    buildAttackMaps(board, ctx);

    evaluatePieces(board, ctx, scores);
    evaluateCentralPawns(board, scores);
    evaluatePassedPawnExtras(board, ctx, passers, scores);
    evaluateBlockedPawns(board, passers, scores);
    evaluateThreats(board, ctx, scores);
    evaluateSpace(board, ctx, scores);
    evaluateKingSafety(board, ctx, scores);

    Score total = scores[White] - scores[Black] + pawnScore;
    total += evaluateInitiative(board, ctx, passers, total);

    int mg = mg_value(total);
    int eg = eg_value(total);

    // Drawish endings scale the endgame half toward zero before blending.
    // Gated on genuine endgame phase so transient opposite-colored-bishop
    // configurations that pop up in the middlegame search tree do not
    // have their eg half distorted; in a true endgame the term still
    // correctly pushes drawish material toward a draw. Specialized
    // endgame patterns (KBNK corner-push) inject an additive eg
    // adjustment ahead of the scale so the gradient survives even when
    // the scaled eg would otherwise flatline.
    if (mgPhase <= 10) {
        // Combine specialized recognizers with the generic OCB / non-OCB
        // scaling rather than replacing it. A handler that returns the
        // default {64, 0} (i.e. "no draw signal here") should not silently
        // disable the generic scaleFactor draw logic that was already
        // covering this material class. For exact-draw recognizers
        // (KPsK fortress, KPK bitbase draw) the specialized handler
        // returns a scale below 64 and the min() collapses to the
        // tighter result; for the rest, the generic OCB damping still
        // gets to apply. egAdjust is purely additive (Lucena bonus,
        // KBNK corner-push gradient, etc.) and rides on top.
        int scale = scaleFactor(board);
        int egAdjust = 0;
        if (const auto *se = Endgame::probeScale(board.materialKey)) {
            Endgame::ScaleResult r = se->fn(board, se->strongSide);
            scale = std::min(scale, r.scale);
            egAdjust += r.egAdjust;
        }
        eg = (eg + egAdjust) * scale / 64;
    }

    int result = (mg * mgPhase + eg * egPhase) / 24;

    // Scale evaluation toward 0 as the halfmove clock approaches 100 so the
    // engine prefers moves that make progress (captures, pawn pushes) and
    // avoids blundering into 50-move rule draws beyond the search horizon.
    // Skip when no pawns remain: in pawnless endgames like KQ vs K the winning
    // side has no way to reset the clock other than capturing, so the search
    // tree itself must handle the 50-move horizon without penalizing the eval.
    if (board.byPiece[Pawn]) {
        result = result * (200 - board.halfmoveClock) / 200;
    }

    // evalParams.Tempo is applied after the halfmove scale so the side-to-move bonus
    // does not decay with the fifty-move counter, then folded into the
    // white-perspective total before the final flip.
    int tempoContribution = mg_value(evalParams.Tempo) * mgPhase / 24;
    result += (board.sideToMove == White) ? tempoContribution : -tempoContribution;

    return (board.sideToMove == White) ? result : -result;
}

void evaluateVerbose(const Board &board, std::ostream &os) {
    ensureEvalInit();

    // Replay the same computation as evaluate() but snapshot each term
    // separately so the breakdown can be printed per side.
    Score pstScores[2] = {0, 0};
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;
        int idx;
        if (p.type == Pawn) {
            idx = (p.color == White) ? sq : (sq ^ 56);
        } else {
            int rrank = (p.color == White) ? squareRank(sq) : (7 - squareRank(sq));
            int file = squareFile(sq);
            int fIdx = std::min(file, 7 - file);
            idx = (rrank << 2) | fIdx;
        }
        pstScores[p.color] += PST[p.type][idx];
    }
    Score pstScore = pstScores[White] - pstScores[Black];

    Score matScores[2];
    int gamePhase = 0;
    evaluateMaterial(board, matScores, gamePhase);
    Score matScore = matScores[White] - matScores[Black];

    Score pawnPerSide[2] = {0, 0};
    Bitboard passers[2] = {0, 0};
    computePawns(board, pawnPerSide, passers);
    Score pawnScore = pawnPerSide[White] - pawnPerSide[Black];

    EvalContext ctx;
    Bitboard whitePawnsCtx = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawnsCtx = board.byPiece[Pawn] & board.byColor[Black];
    ctx.pawnAttacks[White] = pawnAttacksBB(whitePawnsCtx, White);
    ctx.pawnAttacks[Black] = pawnAttacksBB(blackPawnsCtx, Black);
    ctx.mobilityArea[White] = ~board.byColor[White] & ~ctx.pawnAttacks[Black];
    ctx.mobilityArea[Black] = ~board.byColor[Black] & ~ctx.pawnAttacks[White];
    buildAttackMaps(board, ctx);

    Score pieceScores[2] = {0, 0};
    evaluatePieces(board, ctx, pieceScores);
    Score pieceScore = pieceScores[White] - pieceScores[Black];

    Score centerScores[2] = {0, 0};
    evaluateCentralPawns(board, centerScores);
    Score centerScore = centerScores[White] - centerScores[Black];

    Score passerExtrasScores[2] = {0, 0};
    evaluatePassedPawnExtras(board, ctx, passers, passerExtrasScores);
    Score passerExtrasScore = passerExtrasScores[White] - passerExtrasScores[Black];

    Score blockedPawnScores[2] = {0, 0};
    evaluateBlockedPawns(board, passers, blockedPawnScores);
    Score blockedPawnScore = blockedPawnScores[White] - blockedPawnScores[Black];

    Score threatScores[2] = {0, 0};
    evaluateThreats(board, ctx, threatScores);
    Score threatScore = threatScores[White] - threatScores[Black];

    Score spaceScores[2] = {0, 0};
    evaluateSpace(board, ctx, spaceScores);
    Score spaceScore = spaceScores[White] - spaceScores[Black];

    Score kingSafetyScores[2] = {0, 0};
    evaluateKingSafety(board, ctx, kingSafetyScores);
    Score kingSafetyScore = kingSafetyScores[White] - kingSafetyScores[Black];

    Score totalBeforeInitiative = pstScore + matScore + pieceScore + centerScore +
                                  passerExtrasScore + blockedPawnScore + threatScore + spaceScore +
                                  kingSafetyScore + pawnScore;
    Score initiativeScore = evaluateInitiative(board, ctx, passers, totalBeforeInitiative);
    Score total = totalBeforeInitiative + initiativeScore;
    int mg = mg_value(total);
    int eg = eg_value(total);

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;

    int scale = 64;
    int scaledEg = eg;
    int egAdjust = 0;
    if (mgPhase <= 10) {
        scale = scaleFactor(board);
        if (const auto *se = Endgame::probeScale(board.materialKey)) {
            Endgame::ScaleResult r = se->fn(board, se->strongSide);
            scale = std::min(scale, r.scale);
            egAdjust += r.egAdjust;
        }
        scaledEg = (eg + egAdjust) * scale / 64;
    }
    int blended = (mg * mgPhase + scaledEg * egPhase) / 24;

    int halfmoveScaled = blended;
    if (board.byPiece[Pawn]) {
        halfmoveScaled = blended * (200 - board.halfmoveClock) / 200;
    }

    int tempoContribution = mg_value(evalParams.Tempo) * mgPhase / 24;
    int whitePovResult =
        halfmoveScaled + ((board.sideToMove == White) ? tempoContribution : -tempoContribution);
    int stmResult = (board.sideToMove == White) ? whitePovResult : -whitePovResult;

    // Render one eval half (mg or eg) as a pawns-with-two-decimals string
    // padded to exactly 6 chars. One pawn is worth 228 internal units in
    // our score grid so the divisor matches the cp conversion used by the
    // "Total (stm)" line.
    auto fmtPawns = [](int v) {
        std::ostringstream s;
        double pawns = static_cast<double>(v) / 228.0;
        s << std::fixed << std::setprecision(2) << std::showpos << std::setw(6) << pawns;
        return s.str();
    };

    auto fmtInt = [](int v) {
        std::ostringstream s;
        s << std::setw(6) << std::right << v;
        return s.str();
    };

    // Each bucket row prints White mg/eg, Black mg/eg, and a Total mg=<int>
    // eg=<int> tail in the exact setw(6)-right format the flat layout used
    // so substring-based tests that key on "mg=    -5 eg=    -2" and the
    // like keep matching unchanged.
    auto bucket = [&](const char *name, Score white, Score black, Score total) {
        os << " " << std::left << std::setw(14) << name << " | " << fmtPawns(mg_value(white))
           << "  " << fmtPawns(eg_value(white)) << " | " << fmtPawns(mg_value(black)) << "  "
           << fmtPawns(eg_value(black)) << " | mg=" << fmtInt(mg_value(total))
           << " eg=" << fmtInt(eg_value(total)) << '\n';
    };

    // Rows whose per-side split is not tracked (material and imbalance
    // already condense into a single white-minus-black entry, initiative
    // is a position-wide scalar). Keep the Total-column mg=/eg= tail in
    // the same format as the split rows so downstream parsing does not
    // need to branch on bucket identity.
    auto totalOnly = [&](const char *name, Score total) {
        os << " " << std::left << std::setw(14) << name
           << " |   ---     ---  |   ---     ---  | mg=" << fmtInt(mg_value(total))
           << " eg=" << fmtInt(eg_value(total)) << '\n';
    };

    os << "rlngin eval breakdown (pawn units; internal = pawns * 228)\n";
    os << "                |     White      |     Black      |     Total\n";
    os << "                |   mg      eg   |   mg      eg   |\n";
    os << "----------------+----------------+----------------+-------------------\n";
    totalOnly("Material", matScore);
    bucket("PST", pstScores[White], pstScores[Black], pstScore);
    bucket("Pawns", pawnPerSide[White], pawnPerSide[Black], pawnScore);
    bucket("Pieces", pieceScores[White], pieceScores[Black], pieceScore);
    bucket("Center", centerScores[White], centerScores[Black], centerScore);
    bucket("Passed extras", passerExtrasScores[White], passerExtrasScores[Black],
           passerExtrasScore);
    bucket("Blocked pawns", blockedPawnScores[White], blockedPawnScores[Black], blockedPawnScore);
    bucket("Threats", threatScores[White], threatScores[Black], threatScore);
    bucket("Space", spaceScores[White], spaceScores[Black], spaceScore);
    bucket("King safety", kingSafetyScores[White], kingSafetyScores[Black], kingSafetyScore);
    totalOnly("Initiative", initiativeScore);
    os << "----------------+----------------+----------------+-------------------\n";
    os << " " << std::left << std::setw(14) << "Sum"
       << " |                |                | mg=" << fmtInt(mg) << " eg=" << fmtInt(eg) << '\n';
    os << " " << std::left << std::setw(14) << "Phase"
       << " " << mgPhase << "/24\n";
    os << " " << std::left << std::setw(14) << "Scale"
       << " eg * " << scale << "/64 = " << scaledEg << '\n';
    os << " " << std::left << std::setw(14) << "Halfmove"
       << " clock=" << board.halfmoveClock << " -> " << halfmoveScaled << '\n';
    os << " " << std::left << std::setw(14) << "Tempo"
       << " " << ((board.sideToMove == White) ? "+" : "-") << tempoContribution << '\n';
    os << " " << std::left << std::setw(14) << "Total (stm)"
       << " internal=" << stmResult << " cp=" << (stmResult * 100 / 228) << '\n';

    // Safety check: the verbose path should never diverge from evaluate().
    int expected = evaluate(board);
    if (expected != stmResult) {
        os << "  WARNING: verbose total " << stmResult << " differs from evaluate() " << expected
           << '\n';
    }
}

void clearPawnHash() {
    pawnHashTable.clear();
}

void setPawnHashSize(size_t mb) {
    pawnHashTable.resize(mb);
}

void clearMaterialHash() {
    materialHashTable.clear();
}

void setMaterialHashSize(size_t mb) {
    materialHashTable.resize(mb);
}
