#include "eval.h"

#include "bitboard.h"
#include "eval_params.h"
#include "material_hash.h"
#include "pawn_hash.h"
#include "zobrist.h"

#include <algorithm>
#include <iomanip>

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

static PawnHashTable pawnHashTable(2);
static MaterialHashTable materialHashTable(1);

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

static void evaluatePawns(const Board &board, Score &out, Bitboard passers[2]) {
    int mgCached = 0, egCached = 0;
    Bitboard cachedWhitePassers = 0, cachedBlackPassers = 0;
    if (pawnHashTable.probe(board.pawnKey, mgCached, egCached, cachedWhitePassers,
                            cachedBlackPassers)) {
        out = S(mgCached, egCached);
        passers[White] = cachedWhitePassers;
        passers[Black] = cachedBlackPassers;
        return;
    }

    Score score = 0;
    Bitboard whitePassers = 0, blackPassers = 0;

    Bitboard whitePawns = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawns = board.byPiece[Pawn] & board.byColor[Black];

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = (c == White) ? whitePawns : blackPawns;
        Bitboard theirPawns = (c == White) ? blackPawns : whitePawns;
        int sign = (c == White) ? 1 : -1;

        Bitboard pawns = ourPawns;
        while (pawns) {
            int sq = popLsb(pawns);
            int r = squareRank(sq);
            int f = squareFile(sq);
            int relRank = (c == White) ? r : (7 - r);

            // Doubled pawn: another friendly pawn ahead on the same file
            bool isDoubled = (ForwardFileBB[c][sq] & ourPawns) != 0;
            if (isDoubled) {
                score += sign * evalParams.DoubledPawnPenalty;
            }

            // Passed pawn: no enemy pawns ahead on same or adjacent files,
            // and no friendly pawn ahead on the same file (rear doubled pawns
            // are not passed)
            if (!isDoubled && !(PassedPawnMask[c][sq] & theirPawns)) {
                score += sign * evalParams.PassedPawnBonus[relRank];
                if (c == White)
                    whitePassers |= squareBB(sq);
                else
                    blackPassers |= squareBB(sq);
            }

            // Isolated pawn: no friendly pawns on adjacent files
            bool isolated = !(AdjacentFilesBB[f] & ourPawns);
            if (isolated) {
                score += sign * evalParams.IsolatedPawnPenalty;
            }

            // Connected pawn: phalanx (same rank, adjacent file) or defended by friendly pawn
            bool phalanx = (ourPawns & AdjacentFilesBB[f] & RankBB[r]) != 0;
            bool defended = (PawnAttacks[c ^ 1][sq] & ourPawns) != 0;
            if (phalanx || defended) {
                score += sign * evalParams.ConnectedPawnBonus[relRank];
            }

            // Backward pawn: not connected, not isolated, all adjacent friendly pawns
            // are ahead, and the stop square is controlled by an enemy pawn
            if (!phalanx && !defended && !isolated) {
                bool noneBelow = !(PawnSpanMask[c ^ 1][sq] & ourPawns);
                if (noneBelow) {
                    int stopSq = (c == White) ? sq + 8 : sq - 8;
                    if (PawnAttacks[c][stopSq] & theirPawns) {
                        score += sign * evalParams.BackwardPawnPenalty;
                    }
                }
            }
        }
    }

    pawnHashTable.store(board.pawnKey, mg_value(score), eg_value(score), whitePassers,
                        blackPassers);
    passers[White] = whitePassers;
    passers[Black] = blackPassers;
    out = score;
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

        Bitboard knights = board.byPiece[Knight] & board.byColor[c];
        while (knights) {
            int sq = popLsb(knights);
            int count = popcount(KnightAttacks[sq] & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Knight][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += evalParams.KnightOutpostBonus;
            }
        }

        Bitboard bishops = board.byPiece[Bishop] & board.byColor[c];
        while (bishops) {
            int sq = popLsb(bishops);
            int count = popcount(bishopAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Bishop][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += evalParams.BishopOutpostBonus;
            }

            Bitboard sameColorSquares =
                (squareBB(sq) & LightSquaresBB) ? LightSquaresBB : DarkSquaresBB;
            int blockingPawns = popcount(ourPawns & sameColorSquares);
            scores[c] += evalParams.BadBishopPenalty * blockingPawns;
        }

        Bitboard kingBB = board.byPiece[King] & board.byColor[c];
        int kingSq = kingBB ? lsb(kingBB) : -1;
        int kingFile = (kingSq >= 0) ? squareFile(kingSq) : -1;
        bool lostShortCastle = (c == White) ? !board.castleWK : !board.castleBK;
        bool lostLongCastle = (c == White) ? !board.castleWQ : !board.castleBQ;

        Bitboard rooks = board.byPiece[Rook] & board.byColor[c];
        while (rooks) {
            int sq = popLsb(rooks);
            int count = popcount(rookAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += evalParams.MobilityBonus[Rook][count];

            Bitboard fileMask = FileBB[squareFile(sq)];
            bool noOurPawns = !(fileMask & ourPawns);
            bool noTheirPawns = !(fileMask & theirPawns);
            if (noOurPawns && noTheirPawns) {
                scores[c] += evalParams.RookOpenFileBonus;
            } else if (noOurPawns) {
                scores[c] += evalParams.RookSemiOpenFileBonus;
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
            if (count <= 3 && kingSq >= 0) {
                int rookFile = squareFile(sq);
                bool sameSide = (kingFile < 4) == (rookFile < kingFile);
                if (sameSide) {
                    Score penalty = evalParams.TrappedRookByKingPenalty;
                    if (lostShortCastle && lostLongCastle) penalty *= 2;
                    scores[c] += penalty;
                }
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

// Count central squares on our side of the board that are safe from enemy
// pawn attacks and not occupied by our own pawns. Squares behind our own
// pawn chain count double: they are already committed territory and
// amplify the bonus Stockfish-style. Scaled quadratically by the number
// of our non-pawn non-king pieces so the term only bites in middlegames
// with enough material to exploit the extra space.
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
        int kingRank = squareRank(kingSq);

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[them];

        int shieldFileMin = std::max(0, kingFile - 1);
        int shieldFileMax = std::min(7, kingFile + 1);

        // Pawn shield, pawn storm, and open file evaluation per shield file
        for (int f = shieldFileMin; f <= shieldFileMax; f++) {
            Bitboard fileMask = FileBB[f];
            Bitboard ourPawnsOnFile = ourPawns & fileMask;
            Bitboard theirPawnsOnFile = theirPawns & fileMask;

            // Pawn shield: find the closest friendly pawn to our back rank.
            // When the file has no friendly pawn, the missing-shield signal
            // is captured by the semi-open / open file penalties below, so
            // no separate penalty is applied here.
            if (ourPawnsOnFile) {
                int pawnSq = (us == White) ? lsb(ourPawnsOnFile) : msb(ourPawnsOnFile);
                int relRank = (us == White) ? squareRank(pawnSq) : (7 - squareRank(pawnSq));

                if (relRank == 1) {
                    scores[us] += evalParams.PawnShieldBonus[0];
                } else if (relRank == 2) {
                    scores[us] += evalParams.PawnShieldBonus[1];
                }
            }

            // Pawn storm: find the most-advanced enemy pawn on this file
            if (theirPawnsOnFile) {
                int stormSq = (us == White) ? lsb(theirPawnsOnFile) : msb(theirPawnsOnFile);
                int distance = std::abs(squareRank(stormSq) - kingRank);
                int idx = std::max(0, 4 - std::min(4, distance));
                scores[us] -= evalParams.PawnStormPenalty[idx];
            }

            // Open and semi-open file penalties
            if (!ourPawnsOnFile && !theirPawnsOnFile) {
                scores[us] += evalParams.OpenFileNearKing;
            } else if (!ourPawnsOnFile) {
                scores[us] += evalParams.SemiOpenFileNearKing;
            }
        }

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

            // Gate the safe-square penalty on the same multi-attacker
            // condition so a king boxed in by its own pawns is not scored
            // as if it were under attack.
            Bitboard kingMoves = KingAttacks[kingSq] & ~board.byColor[us];
            int safeCount = std::min(popcount(kingMoves & ~enemyAttacks), 8);
            scores[us] += evalParams.KingSafeSqPenalty[safeCount];
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
        if (!ourPassers) continue;

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

    // Pawnless minor-only endings reduce to draws unless one side has a
    // material excess of a rook or more. Kings-only positions fall through
    // to the default scale because the PST ordering of king activity is
    // still useful to the search even if the outcome is a formal draw.
    if (!board.byPiece[Pawn]) {
        int wMinors = board.pieceCount[White][Knight] + board.pieceCount[White][Bishop];
        int bMinors = board.pieceCount[Black][Knight] + board.pieceCount[Black][Bishop];
        int wMajors = board.pieceCount[White][Rook] + board.pieceCount[White][Queen];
        int bMajors = board.pieceCount[Black][Rook] + board.pieceCount[Black][Queen];
        bool onlyMinorsEachSide = wMajors == 0 && bMajors == 0;
        if (onlyMinorsEachSide && wMinors <= 1 && bMinors <= 1 && (wMinors + bMinors) >= 1) {
            return 0;
        }
    }

    return 64;
}

// Reward pieces we attack with a less-valuable attacker, hanging pieces,
// weak queens, and enemy pieces hit by a safe pawn push. Threat types are
// indexed by the victim piece so lower-value attackers never double-count
// against equally-valued victims. All terms consume the shared attack
// maps precomputed in buildAttackMaps.
static void evaluateThreats(const Board &board, const EvalContext &ctx, Score scores[2]) {
    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);

        Bitboard theirPieces = board.byColor[them];
        Bitboard theirNonPawnNonKing = theirPieces & ~board.byPiece[Pawn] & ~board.byPiece[King];

        // Threat by pawn: every enemy non-pawn/non-king attacked by one of
        // our pawns contributes a flat bonus. Pawn attacks cannot be
        // reciprocated by a lower-value attacker, so this is strictly
        // additive and cannot be double counted with the minor/rook blocks.
        Bitboard pawnThreats = ctx.pawnAttacks[us] & theirNonPawnNonKing;
        scores[us] += evalParams.ThreatByPawn * popcount(pawnThreats);

        // Threat by minor: our knights or bishops attacking an enemy rook
        // or queen. Index by the victim so the table naturally zeroes out
        // same-value targets and we never credit minor-on-minor threats.
        Bitboard minorAttacks = ctx.attackedBy[us][Knight] | ctx.attackedBy[us][Bishop];
        Bitboard victims = minorAttacks & theirPieces;
        Bitboard minorVsRook = victims & board.byPiece[Rook];
        Bitboard minorVsQueen = victims & board.byPiece[Queen];
        scores[us] += evalParams.ThreatByMinor[Rook] * popcount(minorVsRook);
        scores[us] += evalParams.ThreatByMinor[Queen] * popcount(minorVsQueen);

        // Threat by rook: our rooks attacking an enemy queen.
        Bitboard rookVsQueen = ctx.attackedBy[us][Rook] & theirPieces & board.byPiece[Queen];
        scores[us] += evalParams.ThreatByRook[Queen] * popcount(rookVsQueen);

        // Threat by king: enemy pieces sitting on a square our king attacks
        // and which the enemy does not defend. Pawns and kings are excluded
        // from the set of victims.
        Bitboard kingVictims =
            ctx.attackedBy[us][King] & theirNonPawnNonKing & ~ctx.allAttacks[them];
        scores[us] += evalParams.ThreatByKing * popcount(kingVictims);

        // evalParams.Hanging pieces: enemy non-pawn pieces undefended and reachable
        // by a capture we would willingly make. "Willingly" means either
        // we have a less valuable attacker on the square (pawn attacks
        // qualify directly) or two or more pieces converging on it so the
        // capture-recapture sequence still wins material. This keeps
        // queen-attacks-undefended-rook and similar trade-losing scenarios
        // from falsely firing the hanging bonus.
        Bitboard undefended = theirNonPawnNonKing & ctx.allAttacks[us] & ~ctx.allAttacks[them];
        Bitboard hanging = undefended & (ctx.attackedBy2[us] | ctx.pawnAttacks[us]);
        scores[us] += evalParams.Hanging * popcount(hanging);

        // Weak queen: enemy queen attacked by two or more of our pieces.
        Bitboard weakQueen = theirPieces & board.byPiece[Queen] & ctx.attackedBy2[us];
        if (weakQueen) scores[us] += evalParams.WeakQueen;

        // Safe pawn push threat: single- or double-push targets that are
        // empty, not attacked by enemy pawns, and either not attacked by
        // the enemy at all or defended by our own pieces. From those
        // safe landing squares, compute the pawn attack footprint and
        // bonus every enemy non-pawn/non-king piece that falls under it.
        // Exclude pieces already attacked by our pawns in place so the
        // bonus is not double counted with evalParams.ThreatByPawn.
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
        scores[us] += evalParams.SafePawnPush * popcount(pushVictims);
    }
}

int evaluate(const Board &board) {
    ensureEvalInit();

    Score scores[2] = {0, 0};
    int gamePhase = 0;

    // PST-only accumulation per square; piece material, bishop pair, and
    // imbalance come from the cached material probe below.
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx = (p.color == White) ? sq : (sq ^ 56);
        scores[p.color] += PST[p.type][idx];
    }

    Score matScores[2];
    evaluateMaterial(board, matScores, gamePhase);
    scores[White] += matScores[White];
    scores[Black] += matScores[Black];

    Score pawnScore = 0;
    Bitboard passers[2] = {0, 0};
    evaluatePawns(board, pawnScore, passers);

    EvalContext ctx;
    Bitboard whitePawnsCtx = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawnsCtx = board.byPiece[Pawn] & board.byColor[Black];
    ctx.pawnAttacks[White] = pawnAttacksBB(whitePawnsCtx, White);
    ctx.pawnAttacks[Black] = pawnAttacksBB(blackPawnsCtx, Black);
    ctx.mobilityArea[White] = ~board.byColor[White] & ~ctx.pawnAttacks[Black];
    ctx.mobilityArea[Black] = ~board.byColor[Black] & ~ctx.pawnAttacks[White];
    buildAttackMaps(board, ctx);

    evaluatePieces(board, ctx, scores);
    evaluatePassedPawnExtras(board, ctx, passers, scores);
    evaluateThreats(board, ctx, scores);
    evaluateSpace(board, ctx, scores);
    evaluateKingSafety(board, ctx, scores);

    Score total = scores[White] - scores[Black] + pawnScore;

    int mg = mg_value(total);
    int eg = eg_value(total);

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;

    // Drawish endings scale the endgame half toward zero before blending.
    // Gated on genuine endgame phase so transient opposite-colored-bishop
    // configurations that pop up in the middlegame search tree do not
    // have their eg half distorted; in a true endgame the term still
    // correctly pushes drawish material toward a draw.
    if (mgPhase <= 10) {
        int scale = scaleFactor(board);
        eg = eg * scale / 64;
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
    // separately so the breakdown can be printed.
    Score pstScore = 0;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;
        int idx = (p.color == White) ? sq : (sq ^ 56);
        Score contribution = PST[p.type][idx];
        pstScore += (p.color == White) ? contribution : -contribution;
    }

    Score matScores[2];
    int gamePhase = 0;
    evaluateMaterial(board, matScores, gamePhase);
    Score matScore = matScores[White] - matScores[Black];

    Score pawnScore = 0;
    Bitboard passers[2] = {0, 0};
    evaluatePawns(board, pawnScore, passers);

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

    Score passerExtrasScores[2] = {0, 0};
    evaluatePassedPawnExtras(board, ctx, passers, passerExtrasScores);
    Score passerExtrasScore = passerExtrasScores[White] - passerExtrasScores[Black];

    Score threatScores[2] = {0, 0};
    evaluateThreats(board, ctx, threatScores);
    Score threatScore = threatScores[White] - threatScores[Black];

    Score spaceScores[2] = {0, 0};
    evaluateSpace(board, ctx, spaceScores);
    Score spaceScore = spaceScores[White] - spaceScores[Black];

    Score kingSafetyScores[2] = {0, 0};
    evaluateKingSafety(board, ctx, kingSafetyScores);
    Score kingSafetyScore = kingSafetyScores[White] - kingSafetyScores[Black];

    Score total = pstScore + matScore + pieceScore + passerExtrasScore + threatScore + spaceScore +
                  kingSafetyScore + pawnScore;
    int mg = mg_value(total);
    int eg = eg_value(total);

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;

    int scale = 64;
    int scaledEg = eg;
    if (mgPhase <= 10) {
        scale = scaleFactor(board);
        scaledEg = eg * scale / 64;
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

    auto formatTerm = [](std::ostream &out, const char *name, Score s) {
        int tmg = mg_value(s);
        int teg = eg_value(s);
        out << "  " << std::left << std::setw(14) << name << " mg=" << std::setw(6) << std::right
            << tmg << " eg=" << std::setw(6) << std::right << teg << '\n';
    };

    os << "rlngin eval breakdown (white perspective unless noted)\n";
    formatTerm(os, "Material", matScore);
    formatTerm(os, "PST", pstScore);
    formatTerm(os, "Pawns", pawnScore);
    formatTerm(os, "Pieces", pieceScore);
    formatTerm(os, "Passed extras", passerExtrasScore);
    formatTerm(os, "Threats", threatScore);
    formatTerm(os, "Space", spaceScore);
    formatTerm(os, "King safety", kingSafetyScore);
    os << "  " << std::left << std::setw(14) << "Sum"
       << " mg=" << std::setw(6) << std::right << mg << " eg=" << std::setw(6) << std::right << eg
       << '\n';
    os << "  " << std::left << std::setw(14) << "Phase"
       << " " << mgPhase << "/24\n";
    os << "  " << std::left << std::setw(14) << "Scale"
       << " eg * " << scale << "/64 = " << scaledEg << '\n';
    os << "  " << std::left << std::setw(14) << "Halfmove"
       << " clock=" << board.halfmoveClock << " -> " << halfmoveScaled << '\n';
    os << "  " << std::left << std::setw(14) << "Tempo"
       << " " << ((board.sideToMove == White) ? "+" : "-") << tempoContribution << '\n';
    os << "  " << std::left << std::setw(14) << "Total (stm)"
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
