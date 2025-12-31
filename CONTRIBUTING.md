# Contributing to RADFU

Thank you for your interest in contributing to RADFU!

To ensure compliance with legal and licensing requirements, we ask all contributors to follow
these guidelines.


## Legal and Clean-Room Guidelines

1. Clean-Room Contributions

   All contributions must be made using clean-room methods. Do not include any proprietary
   code, decompiled firmware, or confidential information from third parties. If you are
   unsure, please ask before contributing.

2. Interoperability Focus

   Contributions should be aimed at achieving interoperability with Renesas RA
   microcontrollers as described in our README.md and LEGAL.md files.

3. Legal Compliance

   By contributing, you confirm that your work complies with the AGPL-3.0-or-later license
   and that you have the right to contribute the code under these terms.


## Enforcement and Reversion of Contributions

Please be aware that adherence to these contribution guidelines is mandatory. If it is
discovered that a contribution does not comply with the legal and clean-room requirements
outlined here, the maintainers reserve the right to revert or reject the contribution.

This is to ensure that the project remains legally compliant and open-source for everyone.


## Developer Certificate of Origin (DCO)

All commits must be signed off to certify that you have the right to contribute the code.
This is enforced by an automated DCO check on pull requests.

To sign off your commits, use the -s flag:

    git commit -s -m "Your commit message"

This adds a Signed-off-by line to your commit message, certifying that you agree to the
Developer Certificate of Origin (https://developercertificate.org/).


## Submitting Changes

1. Fork the repository
2. Create a feature branch
3. Make your changes with signed-off commits: git commit -s
4. Run the tests: meson test -C build
5. Submit a pull request
6. Monitor CI results and fix any issues reported by the automated checks

Thank you for helping us maintain a legally sound and open project!
