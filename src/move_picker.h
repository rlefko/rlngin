#ifndef MOVE_PICKER_H
#define MOVE_PICKER_H

#include "bitboard.h"
#include "board.h"
#include "search.h"
#include <cstdint>

// `ScoredMove` is declared in search.h so SearchState can preallocate the
// picker's scratch buffers on the heap. The definition is pulled in via
// the include above.

// Cumulative "attacked by a piece of at most this class" map for the side
// NOT to move, sampled once per search node. `byPawn` is the enemy pawn
// attack set; `byMinor` adds enemy knights and bishops; `byRook` adds
// enemy rooks. Queens and kings are omitted on purpose: the interesting
// question is "is this square attacked by something strictly less valuable
// than the piece sitting on it", and no piece is less valuable than a
// queen (for rook-class threats) in a way move ordering can act on.
// Layered this way, the picker and LMR can answer "is the mover attacked
// by a lesser piece" with one AND against the tier that matches the
// mover's own value class.
struct ThreatMap {
    Bitboard byPawn = 0;
    Bitboard byMinor = 0;
    Bitboard byRook = 0;
};

// Populate `out` with the enemy's layered attack map. Pawn, knight,
// bishop, and rook attack sets are unioned in ascending material order so
// every caller can query the tier that matches the attacked piece.
void buildThreatMap(const Board &board, ThreatMap &out);

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
// compatible with the old single-pass sort. The single deliberate ordering
// change is that bad captures are served after quiets instead of being
// sorted into the same pass, matching the modern convention.
class MovePicker {
  public:
    // Main-search constructor. `board` is captured by reference because the
    // picker may call `isLegalMove` internally, which requires a mutable
    // board. `state` / `ply` drive killer, counter-move, and continuation
    // history lookups. `threats` is optional; when non-null, quiet scoring
    // applies threat-escape bonuses and walk-in penalties on top of the
    // usual history ordering. Passing null keeps the previous behaviour
    // (tests construct pickers without a threat map).
    MovePicker(Board &board, const SearchState &state, int ply, Move ttMove, bool inCheck,
               const ThreatMap *threats = nullptr);

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
    const ThreatMap *threats_ = nullptr;

    // Scratch buffers live on the heap via SearchState so the picker stays
    // small in stack footprint. At MAX_PLY deep recursion the previous
    // inline std::array layout blew the 512KB default std::thread stack
    // on macOS; borrowing per-ply slices keeps every frame in the low
    // hundreds of bytes.
    ScoredMove *caps_;
    int numCaps_ = 0;
    int capCursor_ = 0;

    ScoredMove *quiets_;
    int numQuiets_ = 0;
    int quietCursor_ = 0;

    // Bad captures are recognized while partitioning captures by SEE sign.
    // We keep indices into `caps_` so we can stream them out at the final
    // phase without re-sorting, and without copying the ScoredMove payload.
    int *badCapIdx_;
    int numBadCaps_ = 0;
    int badCapCursor_ = 0;

    void genCaptures();
    void genQuiets();

    // Walk the pre-sorted buffer one entry at a time. Captures and quiets
    // are sorted up front so selection during iteration is just a cursor
    // bump.
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
