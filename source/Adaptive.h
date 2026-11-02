#pragma once

#include "Palettes.h"

#include <vector>

/**
    The Adaptive palette: chosen from the clip, frame after frame, without
    flicker.

    The grid is cut into at most 96×54 blocks on the GPU, one cell of each
    is taken (Shaders.cpp's analysis pass says which, and why not the
    block's mean) and read back; each is a sample, its colour in OKLab and
    its weight its coverage. On the CPU, in double:

    - **Median cut** seeds the palette: the box of samples with the largest
      weighted squared error is split at its weighted median along its widest
      axis, until there are `Colours` boxes. Deterministic: ties in the sort go
      to the earlier sample. The seeds are numbered darkest first.
    - **k-means** (Lloyd's) refines it, **warm-started from last frame's
      centroids**, so entry i is the same entry from one frame to the next,
      at most 16 rounds a frame, stopping when no sample changes cluster. A
      tie goes to the lower entry. An empty cluster is re-seeded at the
      sample farthest from its own centroid.
    - **Never worse than a fresh median cut.** If the warm result's cost (the
      weighted squared distance of every sample to its nearest entry) is above
      this frame's median cut's, the clip has changed under it: the median cut
      is refined instead and its entries matched to the old ones nearest
      first, so the entries that survive keep their numbers.
    - **What is shown** follows that result through a one-pole of `Adapt`
      seconds, and snaps to it once both show the same 8-bit code through the
      DAC or are within kSnap of each other in OKLab: a still clip reaches a
      still palette in a bounded number of frames, and from then on the
      picture is bit for bit the same.
*/
namespace ramdac
{
struct Sample
{
	Lab lab;
	double weight = 0.0;
};

/// The weighted squared distance of every sample to its nearest centroid.
double KMeansCost( const std::vector< Sample >& samples, const std::vector< Lab >& centroids );

/// Up to k centroids by median cut, darkest first. Fewer when there are fewer
/// distinct samples; empty when every weight is 0.
std::vector< Lab > MedianCut( const std::vector< Sample >& samples, int k );

/// Lloyd's rounds from `centroids`, in place. Returns the rounds taken.
int RefineKMeans( const std::vector< Sample >& samples, std::vector< Lab >& centroids, int maxRounds );

class Adaptive
{
public:
	/// OKLab distance under which what is shown snaps to the target.
	static constexpr double kSnap = 1e-3;
	static constexpr int kMaxRounds = 16;

	/// One frame. `rgba` is `count` cells of linear RGB and coverage.
	void Update( const float* rgba, int count, int colours, double tau, double dt, int dacBits );

	/// What is shown, through the DAC, in the entries' own order.
	const Palette& Shown() const
	{
		return shown8;
	}
	const std::vector< Lab >& Target() const
	{
		return target;
	}
	const std::vector< Lab >& ShownLab() const
	{
		return shown;
	}
	/// This frame's cost, and the median cut's it was held to.
	double Cost() const
	{
		return cost;
	}
	double MedianCutCost() const
	{
		return medianCost;
	}
	int Rounds() const
	{
		return rounds;
	}
	bool Reseeded() const
	{
		return reseeded;
	}

	void Reset();

	/// The --adaptive negative control: a fresh median cut every frame, shown
	/// at once. No warm start, no smoothing.
	void SetColdForTest( bool c )
	{
		cold = c;
	}

private:
	std::vector< Sample > samples;
	std::vector< Lab > target;///< the k-means result, in the entries' order
	std::vector< Lab > shown; ///< what follows it
	Palette shown8;
	int k            = 0;
	double cost      = 0.0;
	double medianCost = 0.0;
	int rounds       = 0;
	bool reseeded    = false;
	bool cold        = false;
};

} // namespace ramdac
