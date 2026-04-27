#include "eval.h"

#include "bitboard.h"
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
            int blockingPawns = popcount(ourPawns & sameColorSquares);
            scores[c] += evalParams.BadBishopPenalty * blockingPawns;

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

            if (theirKingRing && (rAtk & theirKingRing)) {
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
        int kingRank = squareRank(kingSq);

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[them];

        int shieldFileMin = std::max(0, kingFile - 1);
        int shieldFileMax = std::min(7, kingFile + 1);

        // Pawn shield score for a hypothetical king on `kf`, summed over
        // `kf` and its two adjacent files. Walks only the shield half of
        // the term; storm and open-file penalties still fire at the
        // actual king position because they reflect immediate danger,
        // while shield is a latent safety signal the engine should
        // associate with a reachable king location rather than only the
        // current one.
        auto shieldScoreAt = [&](int kf) -> Score {
            Score result = 0;
            int fMin = std::max(0, kf - 1);
            int fMax = std::min(7, kf + 1);
            for (int f = fMin; f <= fMax; f++) {
                Bitboard fileMask = FileBB[f];
                Bitboard ourPawnsOnFile = ourPawns & fileMask;
                if (!ourPawnsOnFile) continue;
                int pawnSq = (us == White) ? lsb(ourPawnsOnFile) : msb(ourPawnsOnFile);
                int relRank = (us == White) ? squareRank(pawnSq) : (7 - squareRank(pawnSq));
                if (relRank == 1)
                    result += evalParams.PawnShieldBonus[0];
                else if (relRank == 2)
                    result += evalParams.PawnShieldBonus[1];
            }
            return result;
        };

        // Use the best shield available between the king's current
        // square and any castled square the rules still permit. A king
        // that can castle kingside or queenside already has that
        // shelter in reach, so the static eval should not reward
        // actually playing the castle (the move ordering and king-to-
        // corner PST are enough to pick it up), nor penalize the
        // pre-castle king for having a central-file shield that is
        // really a flank-file shield in disguise. This mirrors the
        // shelter-at-current-or-castled-square convention that top
        // classical evaluators use.
        Score shield = shieldScoreAt(kingFile);
        bool canKingside = (us == White) ? board.castleWK : board.castleBK;
        bool canQueenside = (us == White) ? board.castleWQ : board.castleBQ;
        if (canKingside) {
            Score alt = shieldScoreAt(6);
            if (mg_value(alt) > mg_value(shield)) shield = alt;
        }
        if (canQueenside) {
            Score alt = shieldScoreAt(2);
            if (mg_value(alt) > mg_value(shield)) shield = alt;
        }
        scores[us] += shield;

        // Pawn storm and open file evaluation per shield file at the
        // actual king position. These track danger, not latent safety,
        // so the "best reachable square" trick above does not apply.
        for (int f = shieldFileMin; f <= shieldFileMax; f++) {
            Bitboard fileMask = FileBB[f];
            Bitboard ourPawnsOnFile = ourPawns & fileMask;
            Bitboard theirPawnsOnFile = theirPawns & fileMask;

            // Pawn storm: find the most-advanced enemy pawn on this file.
            // Classify it as blocked when one of our pawns sits directly
            // in front of it from the attacker's pushing direction: a
            // frontally blocked ram cannot open the file without a trade,
            // so it deserves a much smaller penalty than an unblocked
            // ram driving toward the shield.
            if (theirPawnsOnFile) {
                int stormSq = (us == White) ? lsb(theirPawnsOnFile) : msb(theirPawnsOnFile);
                int distance = std::abs(squareRank(stormSq) - kingRank);
                int idx = std::max(0, 4 - std::min(4, distance));
                int stopSq = (us == White) ? stormSq - 8 : stormSq + 8;
                bool blocked = (stopSq >= 0 && stopSq < 64) && (ourPawns & squareBB(stopSq));
                Score penalty =
                    blocked ? evalParams.BlockedPawnStorm[idx] : evalParams.UnblockedPawnStorm[idx];
                scores[us] -= penalty;
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
                eg_value(evalParams.InitiativeTension) * tension +
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

    // Wrong-colored bishop with a single rook-file pawn: the classical
    // drawn ending. The defending king parks in the promotion corner and
    // the bishop can never evict it because the corner square is on a
    // different color. Gated narrowly so this only fires for the book
    // fortress shape (one file of our own pawns on the a or h file, all
    // other pieces off, defender has at most an opposite-coloured bishop).
    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);
        if (board.pieceCount[us][Bishop] != 1) continue;
        if (board.pieceCount[us][Knight] || board.pieceCount[us][Rook] ||
            board.pieceCount[us][Queen])
            continue;
        if (board.pieceCount[them][Pawn]) continue;
        if (board.pieceCount[them][Knight] || board.pieceCount[them][Rook] ||
            board.pieceCount[them][Queen])
            continue;

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        if (!ourPawns) continue;
        Bitboard rookFilePawns = ourPawns & (FileABB | FileHBB);
        if (rookFilePawns != ourPawns) continue;
        bool onA = (ourPawns & FileABB) != 0;
        bool onH = (ourPawns & FileHBB) != 0;
        if (onA && onH) continue;

        int promoSq = onA ? (us == White ? 56 : 0) : (us == White ? 63 : 7);
        bool promoLight = (squareBB(promoSq) & LightSquaresBB) != 0;
        Bitboard ourBishop = board.byPiece[Bishop] & board.byColor[us];
        bool bishopLight = (ourBishop & LightSquaresBB) != 0;
        if (promoLight == bishopLight) continue;

        Bitboard theirKingBB = board.byPiece[King] & board.byColor[them];
        if (!theirKingBB) continue;
        int theirKingSq = lsb(theirKingBB);

        // The defender holds the draw as long as the king can reach the
        // promotion square by the time the lead pawn queens. Compute the
        // worst case pawn distance on our side: the furthest-back pawn
        // needs the most pushes. Bishop colour mismatch alone is not
        // enough; the king has to make it to the corner in time.
        int leadPawn = (us == White) ? lsb(ourPawns) : msb(ourPawns);
        int pushes = (us == White) ? (7 - squareRank(leadPawn)) : squareRank(leadPawn);
        if (chebyshev(theirKingSq, promoSq) <= pushes) return 0;
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

        // Slider on queen: every friendly bishop or rook whose ray to an
        // enemy queen passes through exactly one intermediate piece. Done
        // from the queen's square by first finding the first blockers on
        // each ray, then re-casting with those blockers removed so the
        // second-line attackers show up. Direct attackers are filtered
        // out with "& ~firstDiag" / "& ~firstOrtho" so they are not
        // double counted against the threat-by-minor and threat-by-rook
        // bonuses that already credit the direct attack.
        Bitboard enemyQueens = theirPieces & board.byPiece[Queen];
        Bitboard ourBishops = board.byPiece[Bishop] & board.byColor[us];
        Bitboard ourRooks = board.byPiece[Rook] & board.byColor[us];
        if (enemyQueens && (ourBishops | ourRooks)) {
            Bitboard occ = board.occupied;
            int diagXrays = 0;
            int orthoXrays = 0;
            Bitboard queensIter = enemyQueens;
            while (queensIter) {
                int qSq = popLsb(queensIter);
                if (ourBishops) {
                    Bitboard firstDiag = bishopAttacks(qSq, occ) & occ;
                    Bitboard xraySquaresDiag = bishopAttacks(qSq, occ ^ firstDiag);
                    diagXrays += popcount(xraySquaresDiag & ourBishops & ~firstDiag);
                }
                if (ourRooks) {
                    Bitboard firstOrtho = rookAttacks(qSq, occ) & occ;
                    Bitboard xraySquaresOrtho = rookAttacks(qSq, occ ^ firstOrtho);
                    orthoXrays += popcount(xraySquaresOrtho & ourRooks & ~firstOrtho);
                }
            }
            scores[us] += evalParams.SliderOnQueenBishop * diagXrays;
            scores[us] += evalParams.SliderOnQueenRook * orthoXrays;
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
    evaluatePassedPawnExtras(board, ctx, passers, scores);
    evaluateBlockedPawns(board, passers, scores);
    evaluateThreats(board, ctx, scores);
    evaluateSpace(board, ctx, scores);
    evaluateKingSafety(board, ctx, scores);

    Score total = scores[White] - scores[Black] + pawnScore;
    total += evaluateInitiative(board, ctx, passers, total);

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
    // separately so the breakdown can be printed per side.
    Score pstScores[2] = {0, 0};
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;
        int idx = (p.color == White) ? sq : (sq ^ 56);
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

    Score totalBeforeInitiative = pstScore + matScore + pieceScore + passerExtrasScore +
                                  blockedPawnScore + threatScore + spaceScore + kingSafetyScore +
                                  pawnScore;
    Score initiativeScore = evaluateInitiative(board, ctx, passers, totalBeforeInitiative);
    Score total = totalBeforeInitiative + initiativeScore;
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
