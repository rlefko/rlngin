#ifndef MOVE_PICKER_H
#define MOVE_PICKER_H

#include "board.h"
#include "search.h"
#include <array>
#include <cstdint>

// Scored entry for the picker's internal buffers and for passing results
// back to the search. `histScore` is the quiet butterfly + continuation
// history sum; LMR reads it to avoid recomputing the same lookups. `score`
// is the full ordering score that the picker uses for selection.
struct ScoredMove {
    int score;
    int histScore;
    Move move;
};

// Phases are traversed in order: the picker advances through them each time
// `next()` exhausts a buffer. The qsearch pipeline reuses the same enum and
// simply transitions Done without the killer / quiet / bad-capture stages.
enum class PickPhase : uint8_t {
    TTMove,
    GenCaptures,
    GoodCaptures,
    Killer1,
    Killer2,
    CounterMove,
    GenQuiets,
    Quiets,
    BadCaptures,
    Done,
    QsTTMove,
    QsGenCaptures,
    QsCaptures,
    QsGenEvasions,
    QsEvasions,
    QsDone,
};

struct PickedMove {
    Move move;
    int score = 0;
    int histScore = 0;
    PickPhase phase = PickPhase::Done;
};

// Staged move picker with lazy generation. The main-search constructor
// iterates TT move, good captures, killers, counter move, quiets, then bad
// captures. The quiescence constructor skips killers / quiets and stops
// after the capture phase (or evasions when in check). All existing
// heuristics (TT, MVV-LVA + SEE, killer, counter-move, butterfly +
// three-tier continuation history) feed into the same scoring that the
// previous score-and-sort implementation used, so move ordering stays
// bit-identical except for two deliberate changes: bad captures are served
// after quiets instead of being sorted into the same pass, and quiets are
// selected via incremental max-selection instead of a full std::sort so
// partial consumption stays cheap on early cutoffs.
class MovePicker {
public:
    // Main-search constructor. `board` is captured by reference because the
    // picker may call `isLegalMove` internally, which requires a mutable
    // board. `state` / `ply` drive killer, counter-move, and continuation
    // history lookups.
    MovePicker(Board &board, const SearchState &state, int ply, Move ttMove, bool inCheck);

    // Quiescence constructor. The bool tag disambiguates from the
    // main-search constructor; passing `true` selects the qsearch pipeline.
    MovePicker(Board &board, const SearchState &state, int ply, Move ttMove, bool inCheck,
               bool qsearchTag);

    // Fetch the next move in ordering order. Returns false when exhausted.
    // `skipMove` is matched by from/to/promotion and suppresses that exact
    // move across every phase so the singular extension excluded move never
    // leaks back into the search.
    bool next(PickedMove &out, const Move &skipMove = {0, 0, None});

    PickPhase currentPhase() const { return phase_; }

private:
    Board &board_;
    const SearchState &state_;
    int ply_;
    Move ttMove_;
    Move killer1_{};
    Move killer2_{};
    Move counterMove_{};
    PickPhase phase_;
    bool inCheck_;

    // Fixed-capacity scratch buffers. Chess positions max out at around
    // 218 legal moves; 256 is the same cap used elsewhere in the search
    // (MAX_LMR_MOVES) and is comfortably above the theoretical maximum.
    std::array<ScoredMove, 256> caps_{};
    int numCaps_ = 0;
    int capCursor_ = 0;

    std::array<ScoredMove, 256> quiets_{};
    int numQuiets_ = 0;
    int quietCursor_ = 0;

    // Bad captures are recognized while partitioning captures by SEE sign.
    // We keep indices into `caps_` so we can stream them out at the final
    // phase without re-sorting, and without copying the ScoredMove payload.
    std::array<int, 256> badCapIdx_{};
    int numBadCaps_ = 0;
    int badCapCursor_ = 0;

    void genCaptures();
    void genQuiets();

    // Incrementally pick the max-score entry from a buffer. Selection sort
    // is cheaper than a full std::sort when the search cuts off early and
    // matches the Stockfish / Ethereal convention.
    bool selectNextCapture(PickedMove &out);
    bool selectNextQuiet(PickedMove &out);
    bool selectNextBadCapture(PickedMove &out);
};

// Used by the picker to validate a TT move before the search calls
// makeMove on it. Exposed so the main-search loop can reuse the same check
// for other move sources if it ever needs to. A TT entry can legitimately
// collide with another position via a zobrist collision; the check keeps
// the engine from handing makeMove a move that the board cannot support.
bool isPseudoLegalMove(const Board &board, const Move &m);

// Scoring routine shared with the picker. Implemented in move_picker.cpp.
// `ply` of -1 selects the qsearch scoring path (MVV-LVA + capture history
// only; no SEE). `outQuietHistory` receives the butterfly + continuation
// history sum so LMR can reuse it without repeating the lookups.
int scoreMove(const Move &m, const Board &board, const Move &ttMove, int ply,
              const SearchState &state, int *outQuietHistory);

// True when `m` removes material from the board: either a direct capture
// or an en passant pawn capture. Quiet pawn pushes into the en passant
// square do not exist because the target is always occupied by a pawn.
bool isCapture(const Board &board, const Move &m);

// Piece type taken by `m`. Returns `Pawn` for en passant and `None` if the
// move is quiet; callers should gate this with `isCapture`.
PieceType capturedType(const Board &board, const Move &m);

// Sum the multi-ply continuation history contribution for a quiet move at
// the given ply. Exposed because search.cpp builds move scoring in places
// that are not the picker itself (e.g. probcut ordering).
int contHistoryScore(const SearchState &state, int ply, PieceType currPt, int currTo);

#endif
