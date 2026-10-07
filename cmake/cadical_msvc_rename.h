/* Renames for CaDiCaL's C-linkage globals in MSVC builds (cmake/cadical.cmake).
 *
 * kissat and CaDiCaL each embed their own, incompatible copy of the "kitten"
 * sub-solver, so both define kitten_* (and a few other C names). On ELF and
 * Mach-O, cadical.cmake partial-links CaDiCaL and makes every symbol but the
 * ccadical_* API local. MSVC has no partial link, so instead this header is
 * force-included (/FI) into every CaDiCaL translation unit and gives each of
 * those names a dvs_cdk_ prefix. CaDiCaL's C++ symbols need no renaming: they
 * are mangled inside namespace CaDiCaL and cannot meet kissat's C names.
 *
 * The list is every non-C++ global CaDiCaL rel-3.0.0 defines (nm on a Linux
 * build) other than ccadical_*. Not hand-maintained in practice: the MSVC
 * build lists cadical.lib's external symbols after building and fails if any
 * C name other than ccadical_* / dvs_cdk_* is left (check_cadical_symbols.cmake),
 * so a CaDiCaL update that adds one stops the build instead of silently
 * binding to kissat's copy.
 */
#ifndef DVS_CADICAL_MSVC_RENAME_H
#define DVS_CADICAL_MSVC_RENAME_H

#define citten_clause_with_id dvs_cdk_citten_clause_with_id
#define citten_clause_with_id_and_equivalence dvs_cdk_citten_clause_with_id_and_equivalence
#define citten_clause_with_id_and_exception dvs_cdk_citten_clause_with_id_and_exception
#define completely_backtrack_to_root_level dvs_cdk_completely_backtrack_to_root_level
#define ipasir_add dvs_cdk_ipasir_add
#define ipasir_assume dvs_cdk_ipasir_assume
#define ipasir_failed dvs_cdk_ipasir_failed
#define ipasir_init dvs_cdk_ipasir_init
#define ipasir_release dvs_cdk_ipasir_release
#define ipasir_set_learn dvs_cdk_ipasir_set_learn
#define ipasir_set_terminate dvs_cdk_ipasir_set_terminate
#define ipasir_signature dvs_cdk_ipasir_signature
#define ipasir_solve dvs_cdk_ipasir_solve
#define ipasir_val dvs_cdk_ipasir_val
#define kitten_add_prime_implicant dvs_cdk_kitten_add_prime_implicant
#define kitten_assume dvs_cdk_kitten_assume
#define kitten_assume_signed dvs_cdk_kitten_assume_signed
#define kitten_binary dvs_cdk_kitten_binary
#define kitten_clause dvs_cdk_kitten_clause
#define kitten_clause_with_id_and_exception dvs_cdk_kitten_clause_with_id_and_exception
#define kitten_clear dvs_cdk_kitten_clear
#define kitten_compute_clausal_core dvs_cdk_kitten_compute_clausal_core
#define kitten_compute_prime_implicant dvs_cdk_kitten_compute_prime_implicant
#define kitten_current_ticks dvs_cdk_kitten_current_ticks
#define kitten_failed dvs_cdk_kitten_failed
#define kitten_fixed dvs_cdk_kitten_fixed
#define kitten_fixed_signed dvs_cdk_kitten_fixed_signed
#define kitten_flip_and_implicant_for_signed_literal dvs_cdk_kitten_flip_and_implicant_for_signed_literal
#define kitten_flip_literal dvs_cdk_kitten_flip_literal
#define kitten_flip_phases dvs_cdk_kitten_flip_phases
#define kitten_flip_signed_literal dvs_cdk_kitten_flip_signed_literal
#define kitten_init dvs_cdk_kitten_init
#define kitten_no_terminator dvs_cdk_kitten_no_terminator
#define kitten_no_ticks_limit dvs_cdk_kitten_no_ticks_limit
#define kitten_randomize_phases dvs_cdk_kitten_randomize_phases
#define kitten_release dvs_cdk_kitten_release
#define kitten_set_terminator dvs_cdk_kitten_set_terminator
#define kitten_set_ticks_limit dvs_cdk_kitten_set_ticks_limit
#define kitten_shrink_to_clausal_core dvs_cdk_kitten_shrink_to_clausal_core
#define kitten_shuffle_clauses dvs_cdk_kitten_shuffle_clauses
#define kitten_signed_value dvs_cdk_kitten_signed_value
#define kitten_solve dvs_cdk_kitten_solve
#define kitten_status dvs_cdk_kitten_status
#define kitten_trace_core dvs_cdk_kitten_trace_core
#define kitten_track_antecedents dvs_cdk_kitten_track_antecedents
#define kitten_traverse_core_clauses dvs_cdk_kitten_traverse_core_clauses
#define kitten_traverse_core_clauses_with_id dvs_cdk_kitten_traverse_core_clauses_with_id
#define kitten_traverse_core_ids dvs_cdk_kitten_traverse_core_ids
#define kitten_unit dvs_cdk_kitten_unit
#define kitten_value dvs_cdk_kitten_value
#define new_learned_klause dvs_cdk_new_learned_klause
#define stable_if_not_profile_mode_dummy dvs_cdk_stable_if_not_profile_mode_dummy
#define unstable_if_no_profile_mode dvs_cdk_unstable_if_no_profile_mode

#endif /* DVS_CADICAL_MSVC_RENAME_H */
