/*
 *  Winter is a UCI chess engine.
 *
 *  Copyright (C) 2016 Jonas Kuratli, Jonathan Maurer, Jonathan Rosenthal
 *  Copyright (C) 2017-2018 Jonathan Rosenthal
 *
 *  Winter is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Winter is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * search.cc
 *
 *  Created on: Nov 6, 2016
 *      Author: Jonathan
 */

#include "search.h"
#include "evaluation.h"
#include "transposition.h"
#include "data.h"
#include "general/debug.h"
#include "general/bookkeeping.h"
#include "general/feature_indexes.h"
#include <random>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cassert>

using namespace move_features;

namespace {

const int kPV = 0;
const int kNW = 1;

const int kNormalSearchMode = 0;
const int kSamplingSearchMode = 1;
const int kSamplingEvalMode = 2;
int kNodeCountSampleAt = 1000;
int kNodeCountSampleEvalAt = 400;
const int kMaxDepthSampled = 3;

Board sampled_board;
Score sampled_alpha;
int sampled_node_type;
Depth sampled_depth;

const Vec<Score, 4> init_futility_margins() {
  Vec<Score, 4> kFutilityMargins;
  if (false) {
    kFutilityMargins[0] = 0;
    kFutilityMargins[1] = 413;
    kFutilityMargins[2] = 722;
    kFutilityMargins[3] = 1071;
  }
  else {
    kFutilityMargins[0] = 0;
    kFutilityMargins[1] = 500;
    kFutilityMargins[2] = 1000;
    kFutilityMargins[3] = 1500;
  }
  return kFutilityMargins;
}

const Vec<Score, 4> kFutileMargin = init_futility_margins();

std::vector<int> search_weights(kNumMoveProbabilityFeatures);

Array2d<Move, 1024, 2> killers;
Score last_search_score = 0;

bool print_info = true;

struct Sorter {
  bool operator() (Move i, Move j) {
    return (i >> 16) > (j >> 16);
  };
};

bool SwapToFront(std::vector<Move> &moves, const Move move) {
  for (int i = 0; i < moves.size(); i++) {
    if (moves[i] == move) {
      std::swap(moves[i], moves[0]);
      return true;
    }
  }
  return false;
}

std::mt19937_64 rng;
size_t max_ply = 0;
long nodes = 0;
long sample_nodes = 0;
long evaluation_nodes = 0;

Time end_time = now();

inline bool finished(){
  return end_time <= now();
}

inline void end_search_time() {
  end_time = now();
}

inline Time get_infinite_time() {
  return now()+std::chrono::hours(24);
}

template<int Quiescent>
int32_t get_move_priority(const Move move, const Board &board, const Move best) {
  if (move == best)
    return 20000;
  else if (GetMoveType(move) > kCapture) {
    return 11000;
  }
  else if (GetMoveType(move) == kCapture) {
    return 1000 + 10 * GetPieceType(board.get_piece(GetMoveDestination(move)))
                - GetPieceType(board.get_piece(GetMoveSource(move)));
  }
  else if (Quiescent == kQuiescent) {
    return 10;
  }
  else if (move == killers[board.get_num_made_moves()][0]) {
    return 1001;
  }
  else if (move == killers[board.get_num_made_moves()][1]) {
    return 1000;
  }
  return 10;
}

template<int Quiescent>
void SortMoves(std::vector<Move> &moves, Board &board, const Move best_move) {
  for (unsigned int i = 0; i < moves.size(); i++) {
    moves[i] |= (get_move_priority<Quiescent>(moves[i], board, best_move) << 16);
  }
  std::sort(moves.begin(), moves.end(), Sorter());
  for (unsigned int i = 0; i < moves.size(); i++) {
    moves[i] &= 0xFFFFL;
  }
}

template<typename T>
T init() {
  return T(kNumMoveProbabilityFeatures);
}

template<> int init<int>() { return 0; }

template<typename T> inline
void AddFeature(T &s, const int index) {
  s[index] = 1;
}

template<> inline void AddFeature<int>(int &s,  const int index) {
  s += search_weights[index];
}

template<typename T>
T GetMoveWeight(const Move move, Board &board, const Move tt_entry,
                const Move last_move, const Vec<BitBoard, 6> &direct_checks,
                const Vec<BitBoard, 6> &taboo_squares) {
  T move_weight = init<T>();
  if (move == tt_entry) {
    AddFeature<T>(move_weight, kPWIHashMove);
    return move_weight;
  }
  int num_made_moves = board.get_num_made_moves();
  if (move == killers[num_made_moves][0]) {
    AddFeature<T>(move_weight, kPWIKiller);
    return move_weight;
  }
  else if (move == killers[num_made_moves][1]) {
    AddFeature<T>(move_weight, kPWIKiller + 1);
    return move_weight;
  }
  PieceType moving_piece = GetPieceType(board.get_piece(GetMoveSource(move)));
  PieceType target = GetPieceType(board.get_piece(GetMoveDestination(move)));
  if (GetMoveType(move) >= kCapture && (target < moving_piece || target == kNoPiece)) {
    if (!board.NonNegativeSEE(move)) {
      AddFeature<T>(move_weight, kPWISEE);
    }
  }
  target -= target / kKing;//The target cannot be a king, so we ignore that case.
  AddFeature<T>(move_weight, kPWIPieceTypeXTargetPieceType
                            + (moving_piece * 6) + target);
  AddFeature<T>(move_weight, kPWIMoveType + GetMoveType(move));
  AddFeature<T>(move_weight, kPWIMoveSource + kPSTindex[GetMoveSource(move)]);
  AddFeature<T>(move_weight, kPWIMoveDestination + kPSTindex[GetMoveDestination(move)]);
  if (last_move != kNullMove && GetMoveDestination(last_move) == GetMoveDestination(move)) {
    AddFeature<T>(move_weight, kPWICaptureLastMoved);
  }
  if (GetSquareBitBoard(GetMoveDestination(move)) & direct_checks[moving_piece]) {
    AddFeature(move_weight, kPWIGivesCheck);
    if (GetMoveType(move) < kEnPassant && !board.NonNegativeSEE(move)) {
      AddFeature<T>(move_weight, kPWISEE + 1);
    }
  }
  else if (GetMoveType(move) == kNormalMove
      && (GetSquareBitBoard(GetMoveDestination(move)) & taboo_squares[moving_piece])) {
    AddFeature<T>(move_weight, kPWITabooDestination);
  }
  AddFeature<T>(move_weight, kPWIForcingChanges + IsMoveForcing(move) + 2 * IsMoveForcing(last_move));
  return move_weight;
}

void SortMovesML(std::vector<Move> &moves, Board &board, const Move best_move) {
  Move last_move = kNullMove;
  if (board.get_num_made_moves() > 0) {
    last_move = board.get_last_move();
  }
  const Vec<BitBoard, 6> direct_checks = board.GetDirectCheckingSquares();
  const Vec<BitBoard, 6> taboo_squares = board.GetTabooSquares();

  for (unsigned int i = 0; i < moves.size(); i++) {
    moves[i] |= ((10000 + GetMoveWeight<int>(moves[i], board, best_move,
                                             last_move, direct_checks,
                                             taboo_squares)) << 16);
  }
  std::sort(moves.begin(), moves.end(), Sorter());
  for (unsigned int i = 0; i < moves.size(); i++) {
    moves[i] &= 0xFFFFL;
  }
}

void build_pv(Board &board, std::vector<Move> &pv, Depth depth) {
  if (depth == 0) {
    return;
  }
  table::Entry entry = table::GetEntry(board.get_hash());
  bool entry_verified = table::ValidateHash(entry, board.get_hash());
  table::PVEntry pv_entry = table::GetPVEntry(board.get_hash());
  bool pv_entry_verified = table::ValidateHash(pv_entry, board.get_hash());
  if (entry_verified || pv_entry_verified) {
    std::vector<Move> moves = board.GetMoves<kNonQuiescent>();
    for (Move move : moves) {
      if ((move == entry.best_move && entry_verified)
          || (move == pv_entry.best_move && pv_entry_verified)) {
        pv.emplace_back(move);
        board.Make(move);
        build_pv(board, pv, depth-1);
        board.UnMake();
        return;
      }
    }
  }
}

inline bool sufficient_bounds(const Board &board, const table::Entry &entry,
                              const Score alpha, const Score beta,
                              const Depth depth) {
  Score score = entry.get_score(board);
  return entry.depth >= depth
      && ((entry.bound == kExactBound)
          || (entry.bound == kLowerBound && score >= beta)
          || (entry.bound == kUpperBound && score <= alpha));
}

inline bool is_mate_score(const Score score) {
  return (score < kMinScore + 2000) || (score > kMaxScore - 2000);
}

inline bool is_null_move_allowed(const Board &board, const Depth depth) {
  return settings::kUseNullMoves && depth > 1
      && board.get_phase() > 1 * piece_phases[kQueen];// && !board.InCheck();
}

//This tested negative, may revisit in the future.
inline bool cutoff_is_prefetchable(Board &board, const Score alpha, const Score beta,
                                const Depth depth, const std::vector<Move> &moves) {
  for (Move move : moves) {
    board.Make(move);
    search::dec_node_count();
    if (alpha >= 0 && board.IsDraw()) {
      board.UnMake();
      return true;
    }
    table::Entry entry = table::GetEntry(board.get_hash());
    if (table::ValidateHash(entry,board.get_hash())
        && entry.get_score(board) <= alpha
        && sufficient_bounds(board, entry, alpha, beta, depth)) {
      board.UnMake();
      return true;
    }
    board.UnMake();
  }
  return false;
}

}

namespace search {

uint64_t Perft(Board &board, Depth depth) {
  if (depth <= 0) {
    return 1;
  }
  if (depth == 1) {
    return board.GetMoves<kNonQuiescent>().size();
  }
  uint64_t perft_sum = 0;
  std::vector<Move> moves = board.GetMoves<kNonQuiescent>();
  for (Move move : moves) {
    board.Make(move);
    perft_sum += Perft(board, depth-1);
    board.UnMake();
  }
  return perft_sum;
}

template<int Mode>
Score QuiescentSearch(Board &board, Score alpha, Score beta) {
  max_ply = std::max(board.get_num_made_moves(), max_ply);

  table::Entry entry = table::GetEntry(board.get_hash());
  bool valid_hash = table::ValidateHash(entry,board.get_hash());
  if (valid_hash && sufficient_bounds(board, entry, alpha, beta, 0)) {
    return entry.get_score(board);
  }
  bool in_check = board.InCheck();
  Score static_eval = kMinScore;
  if (!in_check) {
    if (Mode == kSamplingEvalMode) {
      evaluation_nodes++;
      if (evaluation_nodes == kNodeCountSampleEvalAt) {
        sampled_board.SetToSamePosition(board);
        end_search_time();
      }
    }

    static_eval = evaluation::ScoreBoard(board);
    if (valid_hash && entry.bound == kLowerBound && static_eval < entry.get_score(board)) {
      static_eval = entry.get_score(board);
    }

    if (static_eval >= beta) {
      return static_eval;
    }
    if (static_eval > alpha) {
      alpha = static_eval;
    }
  }
  std::vector<Move> moves = board.GetMoves<kQuiescent>();
  if (in_check && moves.size() == 0) {
    return kMinScore+board.get_num_made_moves();
  }
  if (table::ValidateHash(entry,board.get_hash())) {
    SortMoves<kQuiescent>(moves, board, entry.best_move);
  }
  else {
    SortMoves<kQuiescent>(moves, board, 0);
  }

  for (Move move : moves) {
    if (!in_check && GetMoveType(move) != kEnPassant && !board.NonNegativeSEE(move)) {
        continue;
    }
    board.Make(move);
    Score score = -QuiescentSearch<Mode>(board, -beta, -alpha);
    board.UnMake();
    if (score >= beta) {
      return beta;
    }
    if (score > alpha) {
      alpha = score;
    }
  }
  return alpha;
}

inline Score get_futility_margin(Depth depth, Score score) {
  if (false && settings::kExperimental) {
    depth--;

    const Score intercept[3] = { 113, 204, 706 };
    const double w_score[3] = { 0, 0.036, 0.371 };
    const double w_abs_score[3] = { 0.009, -0.009, 0.373 };
    return std::round(intercept[depth] + w_score[depth] * score
                                       + w_abs_score[depth] * std::abs(score));
  }
  return kFutileMargin[depth];
}

template<int NodeType, int Mode>
Score AlphaBeta(Board &board, Score alpha, Score beta, Depth depth) {
  assert(board.get_num_made_moves() > 0);
  assert(beta > alpha);
  assert(beta == alpha + 1 || NodeType != kNW);

  Score original_alpha = alpha;
  if (board.IsDraw()) {
    return 0;
  }

  if (depth <= 0) {
    return QuiescentSearch<Mode>(board, alpha, beta);
  }

  table::Entry entry = table::GetEntry(board.get_hash());
  bool valid_entry = table::ValidateHash(entry,board.get_hash());
  if (valid_entry
      && sufficient_bounds(board, entry, alpha, beta, depth) ) {
    return entry.get_score(board);
  }

  bool in_check = board.InCheck();
  Score static_eval = alpha;
  if (NodeType == kNW && beta > kMinScore + 2000 && alpha < kMaxScore - 2000 && !in_check) {
    //Score static_eval;
    if (valid_entry) {
      if (entry.bound == kExactBound) {
        static_eval = entry.get_score(board);
      }
      else {
        static_eval = evaluation::ScoreBoard(board);
        if (entry.bound == kLowerBound && static_eval < entry.get_score(board)) {
          static_eval = entry.get_score(board);
        }
      }
    }
    else {
      static_eval = evaluation::ScoreBoard(board);
    }

    if (NodeType == kNW && depth <= 3) {
      if (false && Mode == kSamplingSearchMode && static_eval > beta
                && depth <= kMaxDepthSampled) {
        sample_nodes++;
        if (sample_nodes == kNodeCountSampleAt) {
          sampled_board.SetToSamePosition(board);
          sampled_depth = depth;
          sampled_node_type = NodeType;
          sampled_alpha = alpha;
          end_search_time();
          return alpha;
        }
      }
      //const Score intercept[3] = { 475, 550, 616 };
      //const double w_abs_score[3] = { 0.33, 0.7, 0.68 };
      //const double w_score[3] = { 0, -0.32, -0.30 };
      const Score intercept[3] = { 106, 257, 288 };
      const double w_abs_score[3] = { 0.169, 0.199, 0.216 };
      const double w_score[3] = { -0.015, -0.05, -0.06 };
      Score margin = std::round(intercept[depth - 1] +
                                static_eval * w_score[depth - 1] +
                                std::abs(static_eval) * w_abs_score[depth - 1]);
      if (settings::kUseScoreBasedPruning && static_eval > beta + margin
          && board.get_phase() > 1 * piece_phases[kQueen]) {
        return beta;
      }
    }
    if (static_eval >= beta && is_null_move_allowed(board, depth)) {
      board.Make(kNullMove);
      Score score = -AlphaBeta<kNW, Mode>(board, -beta, -alpha,
                                    depth - 1 - settings::R);
      board.UnMake();
      if (score >= beta) {
        return beta;
      }
    }
  }
  Vec<BitBoard, 6> checking_squares = board.GetDirectCheckingSquares();

  std::vector<Move> moves = board.GetMoves<kNonQuiescent>();
  if (moves.size() == 0) {
    if (board.InCheck()) {
      return kMinScore+board.get_num_made_moves();
    }
    return 0;
  }

//  This tested negative. Perhaps a retry for before NMP and other cutoffs is required
//  if (false && NodeType == kNW && depth >= 2) {
//    if (cutoff_is_prefetchable(board, -beta, -alpha, depth-1, moves)) {
//      return beta;
//    }
//  }

  if (Mode == kSamplingSearchMode && NodeType == kNW && depth <= kMaxDepthSampled) {
    sample_nodes++;
    if (sample_nodes == kNodeCountSampleAt) {
      sampled_board.SetToSamePosition(board);
      sampled_depth = depth;
      sampled_node_type = NodeType;
      sampled_alpha = alpha;
      end_search_time();
      return alpha;
    }
  }

  Move tt_entry = kNullMove;
  if (valid_entry) {
    tt_entry = entry.best_move;
  }
  else {
    table::PVEntry pv_entry = table::GetPVEntry(board.get_hash());
    if (table::ValidateHash(pv_entry,board.get_hash())) {
      tt_entry = pv_entry.best_move;
    }
  }
  bool moves_sorted = false, swapped = false;
  if (tt_entry != kNullMove) {
    swapped = SwapToFront(moves, tt_entry);
    if (swapped && tt_entry != moves[0]) {
      debug::Error("SwapToFront failed");
    }
  }
  if (!swapped) {
    SortMovesML(moves, board, tt_entry);
    moves_sorted = true;
  }

  Move best_local_move = moves[0];
  if (NodeType == kPV && moves.size() == 1) {
    depth++;
  }

  for (unsigned int i = 0; i < moves.size(); i++) {
    if (i == 1 && !moves_sorted) {
      SortMovesML(moves, board, tt_entry);
      moves_sorted = true;
    }
    Move move = moves[i];
    Depth reduction = 0;
    if (NodeType == kNW && !in_check && depth >= 3 && i >= 4
        && GetMoveType(move) <= kDoublePawnMove
        && !(checking_squares[GetPieceType(board.get_piece(GetMoveSource(move)))]
                                      & GetSquareBitBoard(GetMoveDestination(move)))) {
      reduction = (i >= 8) ? 2 : 1;
    }
    assert(reduction < depth);
    if (NodeType == kNW && settings::kUseScoreBasedPruning
        && depth - reduction <= 3 && static_eval < (alpha - get_futility_margin(depth - reduction, static_eval))//futility_margin *(depth - reduction))
        && GetMoveType(move) < kEnPassant && !in_check
        && !(checking_squares[GetPieceType(board.get_piece(GetMoveSource(move)))]
                              & GetSquareBitBoard(GetMoveDestination(move)))) {
      continue;
    }
    board.Make(move);
    Score score;
    if (NodeType == kNW) {
      score = -AlphaBeta<kNW, Mode>(board, -beta, -alpha, depth - 1 - reduction);
      if (reduction > 0 && score >= beta) {
        score = -AlphaBeta<kNW, Mode>(board, -beta, -alpha, depth - 1);
      }
    }
    else if (i == 0) {
      score = -AlphaBeta<kPV, Mode>(board, -beta, -alpha, depth - 1);
    }
    else {
      score = -AlphaBeta<kNW, Mode>(board, -(alpha+1), -alpha, depth - 1);
      if (score >= (alpha+1)) {
        score = -AlphaBeta<kPV, Mode>(board, -beta, -alpha, depth - 1);
      }
    }
    board.UnMake();
    if (finished()) {
      return alpha;
    }
    if (score >= beta) {
      if (NodeType == kPV) {
        bookkeeping::Inc<0>(i);
      }
      table::SaveEntry(board, move, score, kLowerBound, depth);
      if (GetMoveType(move) < kCapture) {
        int num_made_moves = board.get_num_made_moves();
        if (killers[num_made_moves][0] != move) {
          killers[num_made_moves][1] = killers[num_made_moves][0];
          killers[num_made_moves][0] = move;
        }
      }
      return beta;
    }
    if (score > alpha) {
      if (NodeType == kPV) {
        bookkeeping::Inc<0>(i);
      }
      alpha = score;
      best_local_move = move;
    }
  }
  if (alpha > original_alpha) {
    // We should save any best move which has improved alpha.
    table::SaveEntry(board, best_local_move, alpha, kExactBound, depth);
    table::SavePVEntry(board, best_local_move);
  }
  else if (false) {
    //This tested negative or equal with differing conditions.
    //Intuitively some condition should suffice for this to be an improvement
    table::SaveEntry(board, moves[0], alpha, kUpperBound, depth);
  }
  return alpha;
}

template<int Mode>
Score RootSearchLoop(Board &board, Score alpha, Score beta, Depth current_depth,
                     std::vector<Move> &moves) {
  for (int i = 0; i < moves.size(); i++) {
    board.Make(moves[i]);
    if (i == 0) {
      Score score = -AlphaBeta<kPV, Mode>(board, -beta, -alpha, current_depth - 1);
      board.UnMake();
      if (score <= alpha || score >= beta) {
        return score;
      }
      alpha = score;
    }
    else {
      Score score = -AlphaBeta<kNW, Mode>(board, -(alpha + 1), -alpha, current_depth - 1);
      if (score > alpha) {
        score = -AlphaBeta<kPV, Mode>(board, -beta, -alpha, current_depth - 1);
      }
      board.UnMake();
      if (finished()) {
        return alpha;
      }
      if (score >= beta) {
        auto it = moves.rbegin() + moves.size() - i - 1;
        std::rotate(it, it + 1, moves.rend());
        return beta;
      }
      else if (score > alpha) {
        alpha = score;
        auto it = moves.rbegin() + moves.size() - i - 1;
        std::rotate(it, it + 1, moves.rend());
      }
    }
  }
  table::SaveEntry(board, moves[0], alpha, kExactBound, current_depth);
  table::SavePVEntry(board, moves[0]);
  return alpha;
}

template<int Mode>
Move RootSearch(Board &board, Depth depth){
  // Measure complete search time
  const Time begin = now();
  max_ply = board.get_num_made_moves();
  Score score = 0;
  nodes = 0;
  depth = std::min(depth, settings::kMaxDepth);
  std::vector<Move> moves = board.GetMoves<kNonQuiescent>();
  table::Entry entry = table::GetEntry(board.get_hash());
  Move tt_move = kNullMove;
  if (table::ValidateHash(entry,board.get_hash())) {
    tt_move = entry.best_move;
  }
  SortMovesML(moves, board, tt_move);
  for (Depth current_depth = 1; current_depth <= depth; current_depth++) {
    if(finished()) {
      break;
    }
    if (current_depth == 1) {
      score = RootSearchLoop<Mode>(board, kMinScore, kMaxScore, current_depth, moves);
    }
    else {
      Score delta = 500;
      Score alpha = std::max(score-delta, kMinScore);
      Score beta = std::min(score+delta, kMaxScore);
      SortMovesML(moves, board, moves[0]);
      score = RootSearchLoop<Mode>(board, alpha, beta, current_depth, moves);
      while (!finished() && (score <= alpha || score >= beta)) {
        if (score <= alpha) {
          alpha = std::max(alpha-delta, kMinScore);
        }
        else if (score >= beta) {
          beta = std::min(beta+delta, kMaxScore);
        }
        score = RootSearchLoop<Mode>(board, alpha, beta, current_depth, moves);
        delta *= 2;
      }
    }
    if(!finished()){
      last_search_score = score;
      std::vector<Move> pv;
      build_pv(board, pv, current_depth);
      Time end = now();
      auto time_used = std::chrono::duration_cast<Milliseconds>(end-begin);
      if (print_info) {
        std::cout << "info "  << " depth " << current_depth << " seldepth "
            << (max_ply - board.get_num_made_moves()) << " time "
            << time_used.count() << " nodes " << nodes;
        if (!is_mate_score(score)) {
          std::cout << " score cp "
              << (score / 8);
        }
        else {
          Score m_score = board.get_num_made_moves();
          if (score < 0) {
            m_score = -(score - kMinScore - m_score) / 2;
            std::cout << " score mate " << m_score;
          }
          else {
            m_score = (kMaxScore - score - m_score + 2) / 2;
            std::cout << " score mate " << m_score;
          }
        }
        std::cout << " pv";
        for (Move move : pv) {
          std::cout << " " << parse::MoveToString(move);
        }
        std::cout << std::endl;
      }
    }
  }
  return moves[0];
}

void set_print_info(bool print_info_) {
  print_info = print_info_;
}

Score get_last_search_score() {
  return last_search_score;
}

Move DepthSearch(Board board, Depth depth) {
  end_time = get_infinite_time();
  return RootSearch<kNormalSearchMode>(board, depth);
}

Move TimeSearch(Board board, Milliseconds duration) {
  end_time = now()+duration;
  return RootSearch<kNormalSearchMode>(board, 1000);
}

void inc_node_count() {
  nodes++;
}

void dec_node_count() {
  nodes--;
}

void end_search() {
  end_time = now();
}

void clear_killers() {
  for (size_t i = 0; i < killers.size(); i++) {
    killers[i][0] = 0;
    killers[i][1] = 0;
  }
}

void TrainSearchParamsOrderBased(bool from_scratch) {
  const int scaling = 128;
  set_print_info(false);
  std::vector<double> weights(kNumMoveProbabilityFeatures);
  if (!from_scratch) {
    LoadSearchVariables();
    for (int i = 0; i < kNumMoveProbabilityFeatures; i++) {
      weights[i] = search_weights[i];
    }
  }
  weights[kPWIHashMove] = 2000;//High clean hardcoded value for hash.
  std::vector<Game> games = data::LoadGames();
  double nu = 8.;
  int sampled_positions = 0;
  std::vector<double> sampled_depths(kMaxDepthSampled, 0);
  while (true) {
    clear_killers();
    table::ClearTable();
    kNodeCountSampleAt = 300 + rng() % 200;
    Game game = games[rng() % games.size()];
    end_time = now() + Milliseconds(100);
    Board board = game.board;
    sample_nodes = 0;
    sampled_alpha = kMinScore;
    RootSearch<kSamplingSearchMode>(board, 128);
    if (sampled_alpha == kMinScore) {
      continue;
    }
    end_time = get_infinite_time();
    Move last_move = kNullMove;
    if (sampled_board.get_num_made_moves() > 0) {
      last_move = sampled_board.get_last_move();
    }
    std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
    std::shuffle(moves.begin(), moves.end(), rng);
    SortMovesML(moves, sampled_board, kNullMove);
    std::vector<std::vector<int> > features;
    Vec<BitBoard, 6> direct_checks = sampled_board.GetDirectCheckingSquares();
    Vec<BitBoard, 6> taboo_squares = sampled_board.GetTabooSquares();
    for (int i = 0; i < moves.size(); i++) {
      features.emplace_back(GetMoveWeight<std::vector<int> >(moves[i], sampled_board,
                                                            kNullMove, last_move,
                                                            direct_checks, taboo_squares));
    }
    Score alpha = sampled_alpha - 1;
    Score beta = kMaxScore;
    sampled_positions++;
    sampled_depths[sampled_depth - 1]++;
    for (int i = 0; i < moves.size(); i++) {
      Score score;
      sampled_board.Make(moves[i]);
      score = -AlphaBeta<kPV, kNormalSearchMode>(sampled_board,
                                                 -beta,
                                                 -alpha,
                                                 sampled_depth - 1);
      sampled_board.UnMake();
      double target = 0;
      if (score > alpha) {
        if (score == alpha + 1) {
          continue;
        }
        target = 1;
        alpha = score - 1;
      }
      double final_score = 0;
      for (int idx = 1; idx < kNumMoveProbabilityFeatures; idx++) {
        final_score += features[i][idx] * weights[idx];
      }
      final_score /= scaling;
      double sigmoid = 1 / ( 1 + std::exp(-final_score) );
      double gradient = sigmoid - target;
      for (int idx = 1; idx < kNumMoveProbabilityFeatures; idx++) {
        weights[idx] -= (nu * gradient * features[i][idx]);
      }
    }
    if (sampled_positions % 10 == 0) {
      //Our reference is a king moving into the corner with nothing else special.
      //Everything else is set relative to this situation.
      weights[kPWIMoveType + kEnPassant] = 0;
      weights[kPWIPieceTypeXTargetPieceType + kKing * 6 + kNoPiece - 1] = 0;
      weights[kPWIMoveSource] = 0;
    }
    if (sampled_positions % 1000 == 0) {
      std::cout << "Sampled " << sampled_positions << " positions!" << std::endl;
      std::cout << "Sampled depths:";
      for (int i = 0; i < kMaxDepthSampled; i++) {
        std::cout << " " << sampled_depths[i];
      }
      std::cout << std::endl;
      for (int idx = 0; idx < kNumMoveProbabilityFeatures; idx++) {
        search_weights[idx] = std::round(weights[idx]);
      }
      SaveSearchVariables();
    }
    if (sampled_positions % 50000 == 0) {
      nu /= 2;
      std::cout << "New nu: " << nu << std::endl;
    }
  }
}

void CreateSearchParamDataset(bool from_scratch) {
  if (from_scratch) {
    debug::Error("Dataset creation from scratch not supported at the moment.");
  }
  set_print_info(false);
  std::vector< std::vector <int> > samples;
  std::vector<Game> games = data::LoadGames();
  int sampled_positions = 0;
  int all_above = 0, all_below = 0, too_easy = 0;
  while (samples.size() < 1 * kMillion) {
    clear_killers();
    table::ClearTable();
    kNodeCountSampleAt = 1000 + rng() % 500;
    Game game = games[rng() % games.size()];
    if (game.moves.size() < 25) {
      continue;
    }
    game.set_to_position_after((1 * game.moves.size() / 3)
                               + (rng() % (2 * game.moves.size() / 3)) - 2);
    end_time = now() + Milliseconds(200);
    Board board = game.board;
    sample_nodes = 0;
    sampled_alpha = kMinScore;
    RootSearch<kSamplingSearchMode>(board, 128);
    if (sampled_alpha == kMinScore) {
      continue;
    }
    end_time = get_infinite_time();
    Move last_move = kNullMove;
    if (sampled_board.get_num_made_moves() > 0) {
      last_move = sampled_board.get_last_move();
    }
    std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
    std::shuffle(moves.begin(), moves.end(), rng);
    SortMovesML(moves, sampled_board, kNullMove);
    std::vector<std::vector<int> > features;
    Vec<BitBoard, 6> direct_checks = sampled_board.GetDirectCheckingSquares();
    Vec<BitBoard, 6> taboo_squares = sampled_board.GetTabooSquares();
    for (int i = 0; i < moves.size(); i++) {
      features.emplace_back(GetMoveWeight<std::vector<int> >(moves[i], sampled_board,
                                                             kNullMove,last_move,
                                                             direct_checks, taboo_squares));
    }
    std::vector<Score> scores(features.size());
    int low = 0, high = 0;
    for (int i = 0; i < moves.size(); i++) {
      sampled_board.Make(moves[i]);
      Score score = -AlphaBeta<kNW, kNormalSearchMode>(sampled_board,
                                                       -(sampled_alpha+1),
                                                       -sampled_alpha,
                                                       sampled_depth - 1);
      sampled_board.UnMake();
      scores[i] = score;
      if (score > sampled_alpha) {
        high++;
        if (high > moves.size() / 2) {
          break;
        }
      }
      else {
        low++;
      }
    }
    if (high == 0) {
      all_below++;
      continue;
    }
    else if (low == 0) {
      all_above++;
      continue;
    }
    else if (high > low) {
      too_easy++;
      continue;
    }
    int i = rng() % moves.size();
    Score score;
    sampled_board.Make(moves[i]);
    if (sampled_node_type == kNW) {
      score = scores[i];
    }
    else {
      score = -AlphaBeta<kPV, kNormalSearchMode>(sampled_board,
                                                 -(sampled_alpha+1),
                                                 -sampled_alpha,
                                                 sampled_depth - 1);
    }
    sampled_board.UnMake();
    int target = 0;
    if (score > sampled_alpha) {
      target = 1;
    }
    features[i][0] = target;
    samples.emplace_back(features[i]);
    sampled_positions++;
    if (sampled_positions % 1000 == 0) {
      std::cout << "Sampled " << sampled_positions << " positions!" << std::endl;
      std::cout << "Further " << all_above << " all cut nodes, "
                              << all_below << " all nodes and "
                              << too_easy << " too easy nodes!" << std::endl;
      parse::Save2dVecToCSV<int>(samples, "data/search_param_dataset.csv");
    }
  }
  parse::Save2dVecToCSV<int>(samples, "data/search_param_dataset.csv");
  std::cout << "Finished creating dataset!" << std::endl;
}

void TrainSearchParams(bool from_scratch) {
  const int scaling = 128;
  set_print_info(false);
  std::vector<double> weights(kNumMoveProbabilityFeatures);
  if (!from_scratch) {
    LoadSearchVariables();
    for (int i = 0; i < kNumMoveProbabilityFeatures; i++) {
      weights[i] = search_weights[i];
    }
  }
  weights[kPWIHashMove] = 2000;
  weights[kPWIMoveType + kRookPromotion] = -2000;
  weights[kPWIMoveType + kBishopPromotion] = -2000;
  std::vector<Game> games = data::LoadGames();
  double nu = 4.;
  if (!from_scratch) {
    nu /= 8;
  }
  int sampled_positions = 0;
  int all_above = 0, all_below = 0, too_easy = 0;
  while (true) {
    clear_killers();
    table::ClearTable();
    kNodeCountSampleAt = 800 + rng() % 400;
    Game game = games[rng() % games.size()];
    if (game.moves.size() < 25) {
      continue;
    }
    game.set_to_position_after((2 * game.moves.size() / 3)
                               + (rng() % (game.moves.size() / 3)) - 2);
    end_time = now() + Milliseconds(150);
    Board board = game.board;
    sample_nodes = 0;
    sampled_alpha = kMinScore;
    RootSearch<kSamplingSearchMode>(board, 128);
    if (sampled_alpha == kMinScore) {
      continue;
    }
    end_time = get_infinite_time();
    Move last_move = kNullMove;
    if (sampled_board.get_num_made_moves() > 0) {
      last_move = sampled_board.get_last_move();
    }
    std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
    std::shuffle(moves.begin(), moves.end(), rng);
    SortMovesML(moves, sampled_board, kNullMove);
    std::vector<std::vector<int> > features;
    Vec<BitBoard, 6> direct_checks = sampled_board.GetDirectCheckingSquares();
    Vec<BitBoard, 6> taboo_squares = sampled_board.GetTabooSquares();
    for (int i = 0; i < moves.size(); i++) {
      features.emplace_back(GetMoveWeight<std::vector<int> >(moves[i], sampled_board,
                                                             kNullMove,last_move,
                                                             direct_checks, taboo_squares));
    }
    std::vector<Score> scores(features.size());
    int low = 0, high = 0;
    for (int i = 0; i < moves.size(); i++) {
      sampled_board.Make(moves[i]);
      Score score = -AlphaBeta<kNW, kNormalSearchMode>(sampled_board,
                                                       -(sampled_alpha+1),
                                                       -sampled_alpha,
                                                       sampled_depth - 1);
      sampled_board.UnMake();
      scores[i] = score;
      if (score > sampled_alpha) {
        high++;
        if (high > moves.size() / 2) {
          break;
        }
      }
      else {
        low++;
      }
    }
    if (high == 0) {
      all_below++;
      continue;
    }
    else if (low == 0) {
      all_above++;
      continue;
    }
    else if (high > low) {
      too_easy++;
      continue;
    }
    for (int i = 0; i < moves.size(); i++) {
      if (GetMoveType(moves[i]) == kRookPromotion
          || GetMoveType(moves[i]) == kBishopPromotion) {
        continue;
      }
      Score score;
      sampled_board.Make(moves[i]);
      if (sampled_node_type == kNW) {
        score = scores[i];
      }
      else {
        score = -AlphaBeta<kPV, kNormalSearchMode>(sampled_board,
                                                  -(sampled_alpha+1),
                                                  -sampled_alpha,
                                                  sampled_depth - 1);
      }
      sampled_board.UnMake();
      double target = 0;
      if (score > sampled_alpha) {
        target = 1;
      }
      double final_score = 0;
      for (int idx = 0; idx < kNumMoveProbabilityFeatures; idx++) {
        final_score += features[i][idx] * weights[idx];
      }
      final_score /= scaling;
      double sigmoid = 1 / ( 1 + std::exp(-final_score) );
      double gradient = sigmoid - target;
      for (int idx = 0; idx < kNumMoveProbabilityFeatures; idx++) {
        weights[idx] -= (nu * gradient * features[i][idx]) / (1);
      }
    }
    sampled_positions++;
    if (sampled_positions % 10 == 0) {
      //Our reference is a king moving into the corner with nothing else special.
      //Everything else is set relative to this situation.
      weights[kPWIMoveType + kEnPassant] = 0;
      weights[kPWIPieceTypeXTargetPieceType + kKing * 6 + kNoPiece - 1] = 0;
      weights[kPWIMoveSource] = 0;
    }
    if (sampled_positions % 1000 == 0) {
      std::cout << "Sampled " << sampled_positions << " positions!" << std::endl;
      std::cout << "Further " << all_above << " all cut nodes, "
                              << all_below << " all nodes and "
                              << too_easy << " too easy nodes!" << std::endl;
      for (int idx = 0; idx < kNumMoveProbabilityFeatures; idx++) {
        search_weights[idx] = std::round(weights[idx]);
      }
      SaveSearchVariables();
    }
    if (sampled_positions % 300000 == 0) {
      nu /= 2;
      std::cout << "New nu: " << nu << std::endl;
    }
  }
}

void TrainSearchParamsPairwise(bool from_scratch) {
  set_print_info(false);
  std::vector<double> weights(kNumMoveProbabilityFeatures);
  if (!from_scratch) {
    LoadSearchVariables();
    for (int i = 0; i < kNumMoveProbabilityFeatures; i++) {
      weights[i] = search_weights[i];
    }
  }
  std::vector<Game> games = data::LoadGames();
  double nu = 0.4;
  double lambda = 1.0 / kThousand;
  int sampled_positions = 0;
  std::vector<double> sampled_depths(kMaxDepthSampled, 0);
  while (true) {
    clear_killers();
    table::ClearTable();
    kNodeCountSampleAt = 300 + rng() % 200;
    Game game = games[rng() % games.size()];
    end_time = now() + Milliseconds(100);
    Board board = game.board;
    sample_nodes = 0;
    sampled_alpha = kMinScore;
    RootSearch<kSamplingSearchMode>(board, 128);
    if (sampled_alpha == kMinScore) {
      continue;
    }
    end_time = get_infinite_time();
    Move tt_move = 0;
    table::Entry entry = table::GetEntry(sampled_board.get_hash());
    if (table::ValidateHash(entry, sampled_board.get_hash())) {
      tt_move = entry.best_move;
    }
    else {
      table::PVEntry pv_entry = table::GetPVEntry(sampled_board.get_hash());
      if (table::ValidateHash(pv_entry, sampled_board.get_hash())) {
        tt_move = pv_entry.best_move;
      }
    }
    Move last_move = kNullMove;
    if (sampled_board.get_num_made_moves() > 0) {
      last_move = sampled_board.get_last_move();
    }
    std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
    if (moves.size() <= 1) {
      continue;
    }
    std::shuffle(moves.begin(), moves.end(), rng);
    SortMovesML(moves, board, tt_move);
    Vec<BitBoard, 6> direct_checks = sampled_board.GetDirectCheckingSquares();
    Vec<BitBoard, 6> taboo_squares = sampled_board.GetTabooSquares();
    std::vector<std::vector<int> > features;
    for (int i = 0; i < moves.size(); i++) {
      features.emplace_back(GetMoveWeight<std::vector<int> >(moves[i], sampled_board,
                                                                   tt_move,last_move,
                                                                   direct_checks, taboo_squares));
    }
    std::vector<Score> scores(features.size());
    int above_alpha = 0;
    for (int i = 0; i < moves.size(); i++) {
      sampled_board.Make(moves[i]);
      scores[i] = -AlphaBeta<kPV, kNormalSearchMode>(sampled_board,
                                                 kMinScore,
                                                 kMaxScore,
                                                 sampled_depth - 1);
      sampled_board.UnMake();
      if (scores[i] > sampled_alpha)
        above_alpha++;
    }
    sampled_positions++;
    sampled_depths[sampled_depth - 1]++;
    std::vector<double> gradients(weights.size(), 0);
    double sample_importance = sampled_positions / sampled_depths[sampled_depth-1];
    for (int i = 0; i < moves.size() - 1; i++) {
      int j = i + 1;
      if (scores[i] == scores[j]) {
        continue;
      }
      double pair_importance = sample_importance / j;// * (1.0 + j) / (1.0 + i);
      double z = scores[i] >= scores[j] ? 1 : -1;
      double r = 0;
      for (int k = 0; k < weights.size(); k++) {
        r += weights[k] * (features[i][k] - features[j][k]);
      }
      if (z * r < 1000.0) {
        for (int k = 0; k < weights.size(); k++) {
          gradients[k] += z * pair_importance * (features[i][k] - features[j][k]);
        }
      }
    }
    for (int k = 0; k < weights.size(); k++) {
      weights[k] += nu * (gradients[k] - 2 * lambda * weights[k]);
    }
    if (sampled_positions % 1000 == 0) {
      std::cout << "Sampled " << sampled_positions << " positions!" << std::endl;
      for (int idx = 0; idx < kNumMoveProbabilityFeatures; idx++) {
        search_weights[idx] = std::round(weights[idx]);
      }
      SaveSearchVariables();
    }
    if (sampled_positions % 50000 == 0) {
      nu /= 2;
      std::cout << "New nu: " << nu << std::endl;
    }
  }
}


Score QSearch(Board &board) {
  return QuiescentSearch<kNormalSearchMode>(board, kMinScore, kMaxScore);
}

Board SampleEval(Board board) {
  end_time = now() + Milliseconds(1000000);
  evaluation_nodes = 0;
  RootSearch<kSamplingEvalMode>(board, 128);
  return sampled_board;
}

void SaveSearchVariables() {
  std::ofstream file(settings::kSearchParamFile);
  std::ofstream description_file(settings::kSearchParamExplanationFile);
  int idx = 0;
  for (int i = 0; i < kNumMoveProbabilityFeatures; i++) {
    if (i == kFeatureInfos[idx + 1].idx) {
      idx++;
    }
    file << search_weights[i] << " " << std::endl;
    description_file  << search_weights[i]
                      << " <-- " << kFeatureInfos[idx].info << std::endl;
  }
  file.flush();
  file.close();
  description_file.flush();
  description_file.close();
}

void LoadSearchVariables() {
  std::ifstream file(settings::kSearchParamFile);
  for (size_t i = 0; i < kNumMoveProbabilityFeatures; i++) {
    file >> search_weights[i];
  }
  file.close();
}

void EvaluateScoreDistributions(const int focus) {
  set_print_info(false);
  long count1 = 0, count2 = 0;
  std::vector<Game> games = data::LoadGames(1200000);
  const int max_depth = 3, n_score_bins = 160, score_bin_size = 100,
            n_dif_bins = 800, dif_bin_size = 16;
  std::vector<Array2d<long, n_score_bins, n_dif_bins> > histogram(max_depth);
  for (int i = 0; i < max_depth; i++) {
    for (int j = 0; j < n_score_bins; j++) {
      for (int k = 0; k < n_dif_bins; k++) {
        histogram[i][j][k] = 0;
      }
    }
  }
  int id = 0;
  while (true) {
    id++;
    Game game = games[rng() % games.size()];
    int index = (rng() % (2 * game.moves.size() / 3)) + (game.moves.size() / 3) - 2;
    game.set_to_position_after(index);
    clear_killers();
    table::ClearTable();

    kNodeCountSampleAt = 300 + rng() % 150;
    end_time = now() + Milliseconds(150);
    Board board = game.board;
    sample_nodes = 0;
    sampled_alpha = kMinScore;
    RootSearch<kSamplingSearchMode>(board, 128);
    if (sampled_alpha == kMinScore || sampled_board.InCheck()) {
      continue;
    }
    end_time = get_infinite_time();
    Score score = evaluation::ScoreBoard(sampled_board);
    Score score_bin_idx = score;
    score_bin_idx += score_bin_size / 2;
    score_bin_idx /= score_bin_size;
    score_bin_idx += n_score_bins / 2;
    score_bin_idx = std::max(0, (int)score_bin_idx);
    score_bin_idx = std::min(n_score_bins - 1, (int)score_bin_idx);
    if (focus == 0) {
      for (int depth = 1; depth <= max_depth; depth++) {
        DepthSearch(sampled_board, depth);
        Score ab_score = get_last_search_score();
        Score dif = ab_score - score;
        dif /= dif_bin_size;
        dif += n_dif_bins / 2;
        dif = std::max(0, (int)dif);
        dif = std::min(n_dif_bins - 1, (int)dif);
        histogram[depth - 1][score_bin_idx][dif]++;
      }
    }
    else if (focus == 1) {
      std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
      for (int depth = 1; depth <= max_depth; depth++) {
        for (Move move : moves) {
          if (GetMoveType(move) >= kEnPassant) {
            continue;
          }
          sampled_board.Make(move);
          if (sampled_board.InCheck()) {
            sampled_board.UnMake();
            continue;
          }
          Score ab_score = 0;
          if (depth == 1) {
            ab_score = -QSearch(sampled_board);
          }
          else {
            DepthSearch(sampled_board, depth - 1);
            ab_score = -get_last_search_score();
          }
          sampled_board.UnMake();

          Score dif = ab_score - score;
          dif /= dif_bin_size;
          dif += n_dif_bins / 2;
          dif = std::max(0, (int)dif);
          dif = std::min(n_dif_bins - 1, (int)dif);
          histogram[depth - 1][score_bin_idx][dif]++;
        }
      }
    }
    else if (focus == 2) {
      std::vector<Move> moves = sampled_board.GetMoves<kNonQuiescent>();
      for (int depth = 1; depth <= max_depth; depth++) {
        Score max_dif = kMinScore;
        Score max_forcing_dif = kMinScore;
        for (Move move : moves) {
          bool forcing = false;
          if (GetMoveType(move) >= kEnPassant) {
            forcing = true;
          }
          sampled_board.Make(move);
          if (sampled_board.InCheck()) {
            forcing = true;
          }
          Score ab_score = 0;
          if (depth == 1) {
            ab_score = -QSearch(sampled_board);
          }
          else {
            DepthSearch(sampled_board, depth - 1);
            ab_score = -get_last_search_score();
          }
          sampled_board.UnMake();

          if (forcing) {
            max_forcing_dif = std::max(ab_score - score, max_forcing_dif);
          }
          else {
            max_dif = std::max(ab_score - score, max_dif);

          }
        }
        if (max_dif > max_forcing_dif) {
          count1++;
          max_dif /= dif_bin_size;
          max_dif += n_dif_bins / 2;
          max_dif = std::max(0, (int)max_dif);
          max_dif = std::min(n_dif_bins - 1, (int)max_dif);
          histogram[depth - 1][score_bin_idx][max_dif]++;
        }
        count2++;
      }
    }
    if ((id + 1) % 1000 == 0) {
      std::cout << "Evaluated " << (id+1) << " games!" << std::endl;
    }
    if (id % 1000 == 0) {
      std::vector<std::vector <long> > vhistogram(n_score_bins, std::vector<long>(n_dif_bins + 1));
      for (int i = 0; i < n_score_bins; i++) {
        vhistogram[i][0] = (i - (n_score_bins / 2)) * score_bin_size;
      }
      for (int depth = 0; depth < max_depth; depth++) {
        for (int i = 0; i < n_score_bins; i++) {
          for (int j = 0; j < n_dif_bins; j++) {
            vhistogram[i][j + 1] = histogram[depth][i][j];
          }
        }
        switch (focus) {
          case 2:
            parse::Save2dVecToCSV<long>(vhistogram, "data/max_quiet_hist" + std::to_string(depth + 1) + ".csv");
            std::cout << "Max is quiet in " << count1 << "/" << count2 << std::endl;
            break;
          case 1:
            parse::Save2dVecToCSV<long>(vhistogram, "data/v2quiet_hist" + std::to_string(depth + 1) + ".csv");
            break;
          default:
            parse::Save2dVecToCSV<long>(vhistogram, "data/v2hist" + std::to_string(depth + 1) + ".csv");
        }
      }
    }
  }
}


void EvaluateCaptureMoveValue(int n) {
  std::vector<std::vector<Score> > move_scores(6);
  std::vector<Game> games = data::LoadGames();
  for (int i = 0; i < games.size(); i++) {
    Game game = games[i];
    game.set_to_position_after(0);
    while (game.board.get_num_made_moves() < game.moves.size()) {
      const Move move = game.moves[game.board.get_num_made_moves()];
      if (GetMoveType(move) == kCapture
          && !game.board.GivesCheck(move)) {
        PieceType target = game.board.get_piece(GetMoveDestination(move));
        Score before = evaluation::ScoreBoard(game.board);
        game.forward();
        Score after = -evaluation::ScoreBoard(game.board);
        move_scores[GetPieceType(target)].emplace_back(after - before);
      }
      else if (GetMoveType(move) == kEnPassant
          && !game.board.GivesCheck(move)) {
        Score before = evaluation::ScoreBoard(game.board);
        game.forward();
        Score after = -evaluation::ScoreBoard(game.board);
        move_scores[5].emplace_back(after - before);
      }
      else {
        game.forward();
      }
    }
    if ((i + 1) % 10000 == 0) {
      std::cout << "Evaluated " << (i+1) << " games!" << std::endl;
    }
  }
  for (int i = 0; i < 6; i++) {
    std::sort(move_scores[i].begin(), move_scores[i].end());
  }
  for (int j = 0; j < 6; j++) {
    for (int i = 1; i < n; i++) {
      std::cout << i << ":" << move_scores[j][move_scores[j].size() * i / (n)] << " ";
    }
    std::cout << std::endl << std::endl;
  }
}

}
