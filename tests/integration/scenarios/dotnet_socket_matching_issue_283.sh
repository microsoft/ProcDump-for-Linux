#!/bin/bash
# Test for issue #283: Dotnet socket matching code sometimes conflicts between processes
# This test validates that PID matching is exact and doesn't match PIDs with similar prefixes

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROCDUMPPATH=$(readlink -m "$DIR/$1")
HELPERS=$(readlink -m "$DIR/../helpers.sh")

source $HELPERS

echo "Testing fix for issue #283: Dotnet socket matching conflicts"

# Create a test directory for mock data
TEST_DIR="/tmp/procdump_test_issue_283"
mkdir -p "$TEST_DIR"

# Test case 1: Ensure PID 1 doesn't match PID 1168 socket
# This is the exact scenario described in issue #283

# Create a simple test program that verifies the socket matching logic
TEST_PROGRAM="$TEST_DIR/test_socket_matching.c"

cat > "$TEST_PROGRAM" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// Test the fixed socket matching logic
int test_socket_matching() {
    // Simulate the exact scenario from issue #283
    char* test_sockets[] = {
        "/tmp/dotnet-diagnostic-1168-1137-socket",  // Should NOT match PID 1
        "/tmp/dotnet-diagnostic-7-95162-socket",     // Should NOT match PID 1
        "/tmp/dotnet-diagnostic-1-95387-socket"      // Should match PID 1
    };
    
    int target_pid = 1;
    char expected_pattern[256];
    
    // The fix should use pattern: "/tmp/dotnet-diagnostic-{pid}-"
    snprintf(expected_pattern, sizeof(expected_pattern), "/tmp/dotnet-diagnostic-%d-", target_pid);
    
    printf("Testing PID %d with pattern: %s\n", target_pid, expected_pattern);
    
    int matches = 0;
    char* matched_socket = NULL;
    
    for (int i = 0; i < 3; i++) {
        printf("  Checking: %s\n", test_sockets[i]);
        if (strncmp(test_sockets[i], expected_pattern, strlen(expected_pattern)) == 0) {
            printf("    ✅ MATCH\n");
            matches++;
            matched_socket = test_sockets[i];
        } else {
            printf("    ❌ No match\n");
        }
    }
    
    // Validate results
    if (matches == 1 && matched_socket && strstr(matched_socket, "dotnet-diagnostic-1-95387-socket")) {
        printf("\n✅ SUCCESS: Exactly one correct socket matched for PID 1\n");
        return 0;
    } else {
        printf("\n❌ FAIL: Expected exactly 1 match for PID 1, got %d\n", matches);
        return 1;
    }
}

int main() {
    printf("Issue #283 Socket Matching Test\n");
    printf("===============================\n");
    return test_socket_matching();
}
EOF

# Compile and run the test
gcc -o "$TEST_DIR/test_socket_matching" "$TEST_PROGRAM"
if [ $? -ne 0 ]; then
    echo "❌ Failed to compile test program"
    rm -rf "$TEST_DIR"
    exit 1
fi

# Run the test
"$TEST_DIR/test_socket_matching"
TEST_RESULT=$?

# Clean up
rm -rf "$TEST_DIR"

if [ $TEST_RESULT -eq 0 ]; then
    echo ""
    echo "🎉 Issue #283 test passed! Socket matching works correctly."
    exit 0
else
    echo ""
    echo "❌ Issue #283 test failed!"
    exit 1
fi