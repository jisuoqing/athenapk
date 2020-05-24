//========================================================================================
// AthenaPK - a performance portable block structured AMR astrophysical MHD
// code. Copyright (c) 2020, Athena-Parthenon Collaboration. All rights
// reserved. Licensed under the BSD 3-Clause License (the "LICENSE").
//========================================================================================

// Parthenon headers
#include <parthenon/package.hpp>

#include "athena.hpp"
#include "mesh/domain.hpp"
#include "parthenon/prelude.hpp"
#include "parthenon_manager.hpp"

// AthenaPK headers
#include "../eos/adiabatic_hydro.hpp"
#include "../main.hpp"
#include "../recon/recon.hpp"
#include "hydro.hpp"
#include "rsolvers/riemann.hpp"

using parthenon::CellVariable;
using parthenon::IndexDomain;
using parthenon::IndexRange;
using parthenon::Metadata;
using parthenon::ParArray4D;
using parthenon::ParArrayND;
using parthenon::ParthenonManager;

namespace parthenon {

Packages_t ParthenonManager::ProcessPackages(std::unique_ptr<ParameterInput> &pin) {
  Packages_t packages;
  packages["Hydro"] = Hydro::Initialize(pin.get());
  return packages;
}

} // namespace parthenon

// *************************************************//
// define the "physics" package Hydro, which  *//
// includes defining various functions that control*//
// how parthenon functions and any tasks needed to *//
// implement the "physics"                         *//
// *************************************************//

namespace Hydro {

std::shared_ptr<StateDescriptor> Initialize(ParameterInput *pin) {
  auto pkg = std::make_shared<StateDescriptor>("Hydro");

  Real cfl = pin->GetOrAddReal("parthenon/time", "cfl", 0.3);
  pkg->AddParam<>("cfl", cfl);

  auto eos_str = pin->GetString("hydro", "eos");
  if (eos_str.compare("adiabatic") == 0) {
    Real gamma = pin->GetReal("hydro", "gamma");
    Real dfloor = pin->GetOrAddReal("hydro", "dfloor", std::sqrt(1024 * float_min));
    Real pfloor = pin->GetOrAddReal("hydro", "pfloor", std::sqrt(1024 * float_min));
    AdiabaticHydroEOS eos(pfloor, dfloor, gamma);
    pkg->AddParam<>("eos", eos);
  } else {
    // TODO(pgrete) FAIL
    std::cout << "Whoops, EOS undefined" << std::endl;
  }

  // TODO(pgrete): this needs to be "variable" depending on physics
  int nhydro = 5;
  pkg->AddParam<int>("nhydro", nhydro);
  int nhydro_out = pkg->Param<int>("nhydro");

  std::string field_name = "cons";
  Metadata m({Metadata::Cell, Metadata::Independent, Metadata::FillGhost},
             std::vector<int>({nhydro}));
  pkg->AddField(field_name, m);

  field_name = "prim";
  m = Metadata({Metadata::Cell, Metadata::Derived}, std::vector<int>({nhydro}));
  pkg->AddField(field_name, m);
  //  temporary array
  m = Metadata({Metadata::Cell, Metadata::Derived, Metadata::OneCopy},
               std::vector<int>({nhydro}));
  pkg->AddField("wl", m);
  pkg->AddField("wr", m);

  pkg->FillDerived = ConsToPrim;
  pkg->EstimateTimestep = EstimateTimestep;

  return pkg;
}

// this is the package registered function to fill derived, here, convert the
// conserved variables to primitives
void ConsToPrim(Container<Real> &rc) {
  MeshBlock *pmb = rc.pmy_block;
  auto pkg = pmb->packages["Hydro"];
  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::entire);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::entire);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::entire);
  // TODO(pgrete): need to figure out a nice way for polymorphism wrt the EOS
  auto &eos = pkg->Param<AdiabaticHydroEOS>("eos");
  eos.ConservedToPrimitive(rc, ib.s, ib.e, jb.s, jb.e, kb.s, kb.e);
}

// provide the routine that estimates a stable timestep for this package
Real EstimateTimestep(Container<Real> &rc) {
  MeshBlock *pmb = rc.pmy_block;
  auto pkg = pmb->packages["Hydro"];
  const auto &cfl = pkg->Param<Real>("cfl");
  ParArray4D<Real> prim = rc.Get("prim").data.Get<4>();
  auto &eos = pkg->Param<AdiabaticHydroEOS>("eos");

  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  Real min_dt_hyperbolic = std::numeric_limits<Real>::max();

  auto coords = pmb->coords;
  bool nx2 = (pmb->block_size.nx2 > 1) ? true : false;
  bool nx3 = (pmb->block_size.nx3 > 1) ? true : false;

  Kokkos::parallel_reduce(
      "EstimateTimestep",
      Kokkos::MDRangePolicy<Kokkos::Rank<3>>(
          {kb.s, jb.s, ib.s}, {kb.e + 1, jb.e + 1, ib.e + 1}, {1, 1, ib.e + 1 - ib.s}),
      KOKKOS_LAMBDA(const int k, const int j, const int i, Real &min_dt) {
        Real w[(NHYDRO)];
        w[IDN] = prim(IDN, k, j, i);
        w[IVX] = prim(IVX, k, j, i);
        w[IVY] = prim(IVY, k, j, i);
        w[IVZ] = prim(IVZ, k, j, i);
        w[IPR] = prim(IPR, k, j, i);
        Real cs = eos.SoundSpeed(w);
        min_dt = fmin(min_dt, coords.Dx(parthenon::X1DIR, k, j, i) / (fabs(w[IVX]) + cs));
        if (nx2) {
          min_dt =
              fmin(min_dt, coords.Dx(parthenon::X2DIR, k, j, i) / (fabs(w[IVY]) + cs));
        }
        if (nx3) {
          min_dt =
              fmin(min_dt, coords.Dx(parthenon::X3DIR, k, j, i) / (fabs(w[IVZ]) + cs));
        }
      },
      Kokkos::Min<Real>(min_dt_hyperbolic));
  return cfl * min_dt_hyperbolic;
} // namespace Hydro

// Compute fluxes at faces given the constant velocity field and
// some field "advected" that we are pushing around.
// This routine implements all the "physics" in this example
TaskStatus CalculateFluxes(Container<Real> &rc, int stage) {
  MeshBlock *pmb = rc.pmy_block;
  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  int il, iu, jl, ju, kl, ku;
  jl = jb.s, ju = jb.e, kl = kb.s, ku = kb.e;
  // TODO(pgrete): are these looop limits are likely too large for 2nd order
  if (pmb->block_size.nx2 > 1) {
    if (pmb->block_size.nx3 == 1) // 2D
      jl = jb.s - 1, ju = jb.e + 1, kl = kb.s, ku = kb.e;
    else // 3D
      jl = jb.s - 1, ju = jb.e + 1, kl = kb.s - 1, ku = kb.e + 1;
  }

  ParArray4D<Real> w = rc.Get("prim").data.Get<4>();
  ParArray4D<Real> wl = rc.Get("wl").data.Get<4>();
  ParArray4D<Real> wr = rc.Get("wr").data.Get<4>();
  CellVariable<Real> &cons = rc.Get("cons");
  auto pkg = pmb->packages["Hydro"];
  const int nhydro = pkg->Param<int>("nhydro");
  auto &eos = pkg->Param<AdiabaticHydroEOS>("eos");

  auto coords = pmb->coords;
  // get x-fluxes
  ParArray4D<Real> x1flux = cons.flux[parthenon::X1DIR].Get<4>();

  Kokkos::Profiling::pushRegion("Reconstruct X");
  if (stage == 1) {
    DonorCellX1KJI(pmb, kl, ku, jl, ju, ib.s, ib.e + 1, w, wl, wr);
  } else {
    PiecewiseLinearX1KJI(pmb, kl, ku, jl, ju, ib.s, ib.e + 1, w, wl, wr);
  }
  Kokkos::Profiling::popRegion(); // Reconstruct X

  Kokkos::Profiling::pushRegion("Riemann X");
  RiemannSolver(pmb, kl, ku, jl, ju, ib.s, ib.e + 1, IVX, wl, wr, x1flux, eos);
  Kokkos::Profiling::popRegion(); // Riemann X

  //--------------------------------------------------------------------------------------
  // j-direction
  if (pmb->pmy_mesh->ndim >= 2) {
    ParArray4D<Real> x2flux = cons.flux[parthenon::X2DIR].Get<4>();
    // set the loop limits
    il = ib.s - 1, iu = ib.e + 1, kl = kb.s, ku = kb.e;
    if (pmb->block_size.nx3 == 1) // 2D
      kl = kb.s, ku = kb.e;
    else // 3D
      kl = kb.s - 1, ku = kb.e + 1;
    // reconstruct L/R states at j
    Kokkos::Profiling::pushRegion("Reconstruct Y");
    if (stage == 1) {
      DonorCellX2KJI(pmb, kl, ku, jb.s, jb.e + 1, il, iu, w, wl, wr);
    } else {
      PiecewiseLinearX2KJI(pmb, kl, ku, jb.s, jb.e + 1, il, iu, w, wl, wr);
    }
    Kokkos::Profiling::popRegion(); // Reconstruct Y

    Kokkos::Profiling::pushRegion("Riemann Y");
    RiemannSolver(pmb, kl, ku, jb.s, jb.e + 1, il, iu, IVY, wl, wr, x2flux, eos);
    Kokkos::Profiling::popRegion(); // Riemann Y
  }

  //--------------------------------------------------------------------------------------
  // k-direction

  if (pmb->pmy_mesh->ndim >= 3) {
    ParArray4D<Real> x3flux = cons.flux[parthenon::X3DIR].Get<4>();
    // set the loop limits
    il = ib.s - 1, iu = ib.e + 1, jl = jb.s - 1, ju = jb.e + 1;
    // reconstruct L/R states at k
    Kokkos::Profiling::pushRegion("Reconstruct Z");
    if (stage == 1) {
      DonorCellX3KJI(pmb, kb.s, kb.e + 1, jl, ju, il, iu, w, wl, wr);
    } else {
      PiecewiseLinearX3KJI(pmb, kb.s, kb.e + 1, jl, ju, il, iu, w, wl, wr);
    }
    Kokkos::Profiling::popRegion(); // Reconstruct Z

    Kokkos::Profiling::pushRegion("Riemann Z");
    RiemannSolver(pmb, kb.s, kb.e + 1, jl, ju, il, iu, IVZ, wl, wr, x3flux, eos);
    Kokkos::Profiling::popRegion(); // Riemann Z
  }

  return TaskStatus::complete;
}

} // namespace Hydro
