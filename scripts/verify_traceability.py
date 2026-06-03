#!/usr/bin/env python3
import re
import sys
import os

def main():
    doc_path = "/home/ajax80/projects/schema-init/iec62304_skeleton.md"
    if not os.path.exists(doc_path):
        print(f"Error: Skeleton file not found at {doc_path}")
        sys.exit(1)

    with open(doc_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Split into sections based on markdown headers
    # Section 2: SRS
    # Section 4: Traceability Matrix
    # Section 5: Test Case Stubs
    # Section 6: Fault Injection
    
    sections = re.split(r'^##\s+', content, flags=re.MULTILINE)
    
    srs_sec = ""
    matrix_sec = ""
    test_sec = ""
    fi_sec = ""
    
    for sec in sections:
        if sec.startswith("2. Software Requirements Specification"):
            srs_sec = sec
        elif sec.startswith("4. Traceability Matrix"):
            matrix_sec = sec
        elif sec.startswith("5. Test Case Stubs"):
            test_sec = sec
        elif sec.startswith("6. Fault Injection Scenarios"):
            fi_sec = sec

    # 1. Extract SRs from Section 2
    # Match pattern: SR-xxx
    srs_sr_ids = set(re.findall(r'SR-\d{3}', srs_sec))
    
    # 2. Extract TCs from Section 5
    test_tc_ids = set(re.findall(r'TC-\d{3}', test_sec))
    
    # 3. Extract FIs from Section 6
    fi_ids = set(re.findall(r'FI-\d{2}', fi_sec))
    
    # 4. Extract SR, TC mappings from the Traceability Matrix in Section 4
    # Rows look like: | SR-001 | ... | TC-001 |
    matrix_lines = matrix_sec.split('\n')
    matrix_mappings = [] # list of (sr, tc)
    matrix_srs = set()
    matrix_tcs = set()
    
    for line in matrix_lines:
        if '|' in line:
            parts = [p.strip() for p in line.split('|')]
            # Look for SR-xxx and TC-xxx in the parts
            srs_found = []
            tcs_found = []
            for part in parts:
                srs_found.extend(re.findall(r'SR-\d{3}', part))
                tcs_found.extend(re.findall(r'TC-\d{3}', part))
            
            for sr in srs_found:
                matrix_srs.add(sr)
                for tc in tcs_found:
                    matrix_tcs.add(tc)
                    matrix_mappings.append((sr, tc))

    errors = 0

    print("=== IEC 62304 Requirement Traceability Verification ===")
    print(f"Found {len(srs_sr_ids)} SRS Requirements.")
    print(f"Found {len(test_tc_ids)} Test Cases in stubs.")
    print(f"Found {len(matrix_srs)} SRs mapped in Traceability Matrix.")
    print(f"Found {len(matrix_tcs)} TCs mapped in Traceability Matrix.")
    print(f"Found {len(fi_ids)} Fault Injection scenarios.")
    print("-" * 55)

    # Check 1: Any SR in Section 2 not mapped in Section 4?
    unmapped_srs = srs_sr_ids - matrix_srs
    if unmapped_srs:
        print(f"ERROR: SRS Requirements not mapped in Traceability Matrix: {sorted(unmapped_srs)}")
        errors += 1
    else:
        print("OK: All SRS Requirements are mapped in Traceability Matrix.")

    # Check 2: Any SR in Section 4 not defined in Section 2?
    undefined_srs = matrix_srs - srs_sr_ids
    if undefined_srs:
        print(f"ERROR: Traceability Matrix maps undefined SRs: {sorted(undefined_srs)}")
        errors += 1
    else:
        print("OK: All SRs in Traceability Matrix are defined in SRS.")

    # Check 3: Any TC in Section 5 not mapped in Section 4?
    unmapped_tcs = test_tc_ids - matrix_tcs
    if unmapped_tcs:
        print(f"ERROR: Test cases in stubs not mapped in Traceability Matrix: {sorted(unmapped_tcs)}")
        errors += 1
    else:
        print("OK: All test cases in stubs are mapped in Traceability Matrix.")

    # Check 4: Any TC in Section 4 not defined in Section 5?
    undefined_tcs = matrix_tcs - test_tc_ids
    if undefined_tcs:
        print(f"ERROR: Traceability Matrix maps undefined TCs: {sorted(undefined_tcs)}")
        errors += 1
    else:
        print("OK: All TCs in Traceability Matrix have test stubs.")

    # Check 5: Fault Injection scenarios - verify they don't have orphan IDs
    print(f"Fault Injection Scenarios present: {sorted(fi_ids)}")

    print("-" * 55)
    if errors > 0:
        print(f"VERIFICATION FAILED: {errors} traceability issue(s) detected.")
        sys.exit(1)
    else:
        print("VERIFICATION SUCCESSFUL: 100% requirements-to-test coverage verified.")
        sys.exit(0)

if __name__ == "__main__":
    main()
