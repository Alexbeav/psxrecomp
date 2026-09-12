# Wave 4 source qualification

The candidate starts at upstream 89db8168 with the retained GTE and MDEC
corrections in b198e51e. The Windows qualification uses GCC 16.1 and GNU ld.

The textured-dot test linked with the LLD-specific --error-limit=0 option.
GNU ld rejected that option before the test executable linked. Remove this
diagnostic-only option. Keep section garbage collection and every test assertion.
The unchanged test executable is the regression gate for this correction.

The portfolio SOURCE-PIN.md owns the final source identity and qualification
receipt. This source note does not claim game acceptance or publication.
