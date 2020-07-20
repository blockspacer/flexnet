#!/usr/bin/env bash
echo >>~/iwyu.log include-what-you-use "$@"
exec include-what-you-use "$@"

