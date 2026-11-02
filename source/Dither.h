#pragma once

#include <array>

/**
    The dithers' tables, on the CPU.

    **Bayer.** The rank of a cell in the 2^k × 2^k recursive matrix, from its
    coordinates' bits: the xor of x and y and y itself, interleaved and
    reversed. k = 1 is [[0, 2], [3, 1]].

    **Dot diffusion** is Donald Knuth's (Digital halftones by dot diffusion,
    ACM Transactions on Graphics 6(4), 1987, 245–273): error diffusion made
    parallel. The plane is tiled by an 8×8 class matrix; every cell of class
    k is decided at once, after every class below it, and passes its error to
    its eight neighbours of higher class, weight 2 orthogonally and 1
    diagonally, normalised over those neighbours. A cell with no neighbour of
    higher class -- a baron -- keeps its error, so it is lost. The matrix is
    the paper's, as US 8,077,355 B2 reproduces it as its Table 2; the
    `dot-diff.w` that ships with Knuth's programs computes a different one,
    and is not used.

    The GPU decides a class in one draw, gathering rather than scattering:
    for each class this file lists the lower-class neighbours a cell of it
    receives from, and the share of each one's error it gets.
*/
namespace ramdac::dither
{
constexpr int kClasses = 64;

/// The class matrix, [y][x], row 0 the bottom (GL's order).
extern const int kKnuthClass[ 8 ][ 8 ];

/// One lower-class neighbour of a cell of some class.
struct Donor
{
	int dx = 0, dy = 0;        ///< from the receiving cell to the donor, in cells
	int donorClass = 0;
	int tileDx = 0, tileDy = 0;///< the donor's 8×8 tile, from the receiver's
	double share   = 0.0;      ///< of the donor's error: its weight here over its weights in all
};

struct ClassPlan
{
	int x = 0, y = 0;///< where the class sits in the 8×8 tile
	int donorCount = 0;
	std::array< Donor, 8 > donors {};
};

/// The plan for every class. `swappedWeights` is the --diffusion negative
/// control: 1 orthogonally and 2 diagonally.
std::array< ClassPlan, kClasses > BuildPlan( bool swappedWeights = false );

/// The classes with no neighbour of higher class.
bool IsBaron( int cls );

/// The rank of (x, y) in the 2^k Bayer matrix, 0 .. 4^k - 1.
int BayerRank( int x, int y, int k );

} // namespace ramdac::dither
