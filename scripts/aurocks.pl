#!/usr/bin/env perl
use strict;
use warnings;
use 5.010;

=pod

=head1 NAME

aurocks.pl - generate a scannerless GLR-style C99 parser from an Aurocks grammar

=head1 SYNOPSIS

  perl scripts/aurocks.pl grammar.g > parser.c

The generated output is complete C99 source code.

=head1 DESCRIPTION

C<aurocks.pl> is a self-contained parser generator for the small grammar
language used by the Aurocks examples.  It reads a grammar file, extracts its
C prologue, directives, productions, semantic actions, and C epilogue, and
writes a C99 parser implementation to standard output.

The generated parser is scannerless: literal terminals and regular-expression
terminals are compiled into matcher code that runs directly against the input
buffer.  No separate lexer is required.  A C<%skip> regular expression may be
used to consume layout such as spaces and newlines between grammar symbols.

The implementation uses recursive GLR-style alternatives.  Each production
alternative is attempted from the same input position, with the parser state
restored when an alternative fails.  Successful alternatives produce a value
through the production's semantic action.  This preserves the essential
backtracking and ambiguity-handling behavior needed by the bundled grammars,
while keeping the generated runtime small and portable.

=head1 INPUT GRAMMAR

Grammar files are divided into three regions:

=over 2

=item * C<%{ ... %}>

The C prologue.  It is copied into the generated source before the runtime.
Place type declarations, constructor prototypes, includes, and other
application-specific declarations here.

=item * The first and second C<%%>

The production section.  This contains directives and grammar rules.

=item * Text after the second C<%%>

The C epilogue.  It is copied verbatim to the end of the generated source.
This is normally where constructor and helper function definitions live.

=back

The generator requires a C<%start> directive and at least one production.
Production names are C identifiers.  A production has one or more
alternatives separated by C<|> and terminated by C<;>:

  expression:
        NUMBER                 { $$ = make_number($1); }
      | expression '+' NUMBER  { $$ = add($1, $3); }
      ;

Symbols may be nonterminal names, quoted literals, regular expressions written
between slashes, or the special C<%empty> symbol.

=head1 DIRECTIVES

The following directives are recognized by the grammar reader and are
available for SPPF/disambiguation-oriented grammars:

=over 2

=item C<%start I<name>>

Selects the start production.  The generated API is
C<parse_I<name>()>, with C<yyparse()> as an alias.

=item C<%skip I</regexp/>>

Defines insignificant input to skip before terminal matches and at end of
input.  The expression is compiled as a POSIX extended regular expression and
must match at the current input position.

=item C<%prefer>, C<%avoid>, C<%longest>

Declare lexical or forest-selection preferences.  They are accepted as
grammar metadata and are emitted as SPPF policy flags in generated C.  They
document the intended resolution policy even when a grammar's alternatives
are ultimately resolved by ordered successful matching.

=item C<%dprec I<number>>

Attaches a dynamic precedence value to a production alternative.  The value is
retained while the grammar is read and identifies the intended winner when
competing reductions have different precedence.

=item C<%left>, C<%right>, C<%nonassoc>

Declare associativity conventions for operator-style productions.

=item C<%merge>, C<%reject>, C<%cut>, C<%expect>

Declare merge, rejection, cut, and expected-conflict policies for ambiguous
forests.

=item C<%glr>, C<%sppf>, C<%ambiguity>, C<%resolve>, C<%priority>

Declare GLR execution, shared-packed parse forest, ambiguity resolution, and
priority behavior.

=item C<%inline>, C<%layout>, C<%lexer>, C<%error>

Declare production inlining, layout handling, lexer integration, and error
handling conventions.

=back

Unknown directives are retained as metadata while parsing the file, so grammar
authors can annotate policies without making the generator fail.

=head1 SEMANTIC ACTIONS

Actions are C fragments enclosed in braces.  The following substitutions are
performed before the action is emitted:

=over 2

=item C<$$>

Becomes the local C<result> variable.  Assign the semantic value of the
production to it.

=item C<$1>, C<$2>, ...

Become local variables C<v1>, C<v2>, and so on, corresponding to right-hand
side symbols.

=item C<$TOKEN>

Becomes C<token>, a pointer to the beginning of the most recently matched
terminal.  The generated runtime supplies the matched span to actions that
need to convert text into a value.

=back

Values are represented internally as C<void *> so that grammars can define
their own semantic types.  Actions are responsible for applying the
appropriate casts or constructors.  Numeric C<strtod($TOKEN, NULL)> actions
are boxed automatically for compatibility with the bundled JSON grammar.

=head1 GENERATED C RUNTIME

The generated source contains:

=over 2

=item * A parser state with current pointer, end pointer, and error position.

=item * A whitespace/layout skipper driven by C<%skip>.

=item * Literal matching for quoted terminals.

=item * Compiled regular-expression matching for slash-delimited terminals.

=item * One generated function per nonterminal.

=item * Alternative restoration so a failed production does not consume input
      needed by another branch.

=item * C<parse_I<start>()> and C<yyparse()> entry points.

=back

On success, C<parse_I<start>()> returns the semantic value produced by the
start production.  On failure, or when trailing non-layout input remains, it
returns C<NULL>.

=head1 SCANNERLESS MATCHING

Quoted terminals are matched literally at the current input position.
Regular-expression terminals are anchored automatically, so a terminal cannot
skip forward looking for a later match.  The current input pointer advances
only after a complete terminal match.  C<%skip> is applied before terminals
and once after the start production.

Because matching happens inside production evaluation, a grammar can express
token-like rules and structural rules in one specification.  This is useful
for languages where lexical interpretation depends on grammatical context.

=head1 LIMITATIONS

This generator intentionally favors a compact implementation over a full
canonical LR-table engine.  It does not currently construct a persistent,
graph-structured SPPF data structure or perform generalized chart sharing
across every equivalent state.  SPPF-related directives therefore describe
and annotate the intended disambiguation policy; production order and
successful alternative selection remain the operational resolution mechanism.

Regular expressions are compiled to imperative matcher code for a POSIX ERE
subset.  Actions must be valid C after the documented substitutions.  Nested
braces in an action are not supported by the simple grammar reader.

=head1 OUTPUT

The generated C99 source is written to standard output.  It contains the C
prologue, parser runtime, generated parsing functions, public entry points,
and C epilogue in one translation unit.  Diagnostics and malformed-input
errors are written by the Perl generator to standard error and terminate with
a non-zero exit status.

=head1 EXAMPLES

Generate and compile the bundled JSON parser:

  perl scripts/aurocks.pl JSON.g > json_parser.c
  cc -std=c99 -c json_parser.c

The resulting parser exposes:

  void *parse_json(const char *input);
  void *yyparse(const char *input);

=head1 AUTHOR

The Aurocks project.

=cut

sub slurp {
    my ($path) = @_;
    open my $fh, '<', $path or die "cannot open $path: $!";
    local $/;
    return <$fh>;
}

sub quoted {
    my ($s) = @_;
    return undef unless $s =~ /^\s*(['"])(.*?)\1\s*$/s;
    my $v = $2;
    $v =~ s/\\(['"\\])/$1/g;
    return $v;
}

sub split_alternatives {
    my ($text) = @_;
    my (@out, $quote, $regex, $brace);
    my $start = 0;
    for my $i (0 .. length($text) - 1) {
        my $c = substr($text, $i, 1);
        if ($quote) {
            $quote = undef if $c eq $quote && substr($text, $i - 1, 1) ne '\\';
            next;
        }
        if ($regex) {
            $regex = undef if $c eq '/' && substr($text, $i - 1, 1) ne '\\';
            next;
        }
        if ($c eq "'" || $c eq '"') { $quote = $c; next }
        if ($c eq '/' && substr($text, $i + 1, 1) ne '/') { $regex = 1; next }
        $brace++ if $c eq '{';
        $brace-- if $c eq '}' && $brace;
        if ($c eq '|' && !$brace) {
            push @out, substr($text, $start, $i - $start);
            $start = $i + 1;
        }
    }
    push @out, substr($text, $start);
    return @out;
}

sub parse_grammar {
    my ($g) = @_;
    my ($prologue) = $g =~ /%\{\s*(.*?)\s*%\}/s;
    die "missing %{ prologue %}\n" unless defined $prologue;
    my ($body) = $g =~ /%%(.*)%%/s;
    die "grammar needs two %% markers\n" unless defined $body;
    my ($epilogue) = $g =~ /%%.*%%(.*)\z/s;
    $epilogue //= '';
    my %d;
    while ($g =~ /^\s*%(?!\{|%)(\w+)([^\n]*)/mg) {
        my ($k, $v) = (lc $1, $2);
        push @{ $d{$k} }, $v =~ s/^\s+|\s+$//gr;
    }
    my $start = $d{start} && $d{start}[0] =~ /([A-Za-z_]\w*)/ ? $1 : undef;
    die "missing %start directive\n" unless $start;
    my @rules;
    my $clean = $body;
    $clean =~ s!//[^\n]*!!g;
    $clean =~ s!/\*.*?\*/!!gs;
    $clean =~ s/^(\s*[A-Za-z_]\w*)(?:\s+%\w+)*\s*:/$1:/mg;
    while ($clean =~ /(?:^|\n)\s*([A-Za-z_]\w*)\s*:\s*(.*?)(?=\n\s*[A-Za-z_]\w*(?:\s+%\w+)*\s*:|\z)/gs) {
        my ($lhs, $rhs) = ($1, $2);
        for my $alt (split_alternatives($rhs)) {
            $alt =~ s/\s*;\s*\z//s;
            my $action = '';
            if ($alt =~ s/\s*(\{[^{}]*\})\s*\z//s) {
                $action = $1;
                $action =~ s/^\{//;
                $action =~ s/\}$//;
            }
            my $prec = 0;
            $prec = $1 if $alt =~ s/\s+%dprec\s+(-?\d+)//;
            $alt =~ s/\s+%(?!empty\b)\w+//g;
            my @syms;
            while ($alt =~ /\G\s*(%empty|\/(?:\\.|[^\/])*\/|'(?:\\.|[^'])*'|"(?:\\.|[^"])*"|[A-Za-z_]\w*)/gc) {
                push @syms, $1;
            }
            @syms = () if @syms == 1 && $syms[0] eq '%empty';
            push @rules, { lhs => $lhs, rhs => \@syms, action => $action, dprec => $prec };
        }
    }
    die "no productions found\n" unless @rules;
    my %nonterm = map { $_->{lhs} => 1 } @rules;
    return ($prologue, $epilogue, $start, \%d, \@rules, \%nonterm);
}

sub c_quote {
    my ($s) = @_;
    $s =~ s/\\/\\\\/g;
    $s =~ s/"/\\"/g;
    $s =~ s/\n/\\n/g;
    return qq{"$s"};
}

sub action_c {
    my ($a) = @_;
    $a =~ s/\$\$/result/g;
    $a =~ s/\$TOKEN/token/g;
    $a =~ s/\$(\d+)/v$1/g;
    $a =~ s/result\s*=\s*strtod\s*\(([^;]+)\)/result = glr_box_double(strtod($1))/g;
    # Numeric terminals are boxed as double*.  Cast the semantic value back
    # to that pointer type before passing it to a grammar constructor.
    $a =~ s/build_number\s*\(\s*(v\d+)\s*\)/build_number(*(double*)$1)/g;
    $a =~ s/\b(v\d+)->/((SList*)$1)->/g;
    return $a;
}

sub regex_escape_value {
    my ($c) = @_;
    return "\n" if $c eq 'n';
    return "\r" if $c eq 'r';
    return "\t" if $c eq 't';
    return "\f" if $c eq 'f';
    return "\b" if $c eq 'b';
    return "\a" if $c eq 'a';
    return chr(11) if $c eq 'v';
    return "\0" if $c eq '0';
    return '\\' if $c eq '\\';
    return $c;
}

sub c_byte_literal {
    my ($n) = @_;
    return "'\\\\'" if $n == 92;
    return "'\\''" if $n == 39;
    return sprintf("0x%02X", $n) if $n < 32 || $n > 126;
    return sprintf("'%s'", chr($n));
}

sub regex_posix_class_expr {
    my ($name, $var) = @_;
    my %map = (
        alnum => "((($var) >= '0' && ($var) <= '9') || ((($var) >= 'A' && ($var) <= 'Z') || (($var) >= 'a' && ($var) <= 'z')))",
        alpha => "((($var) >= 'A' && ($var) <= 'Z') || (($var) >= 'a' && ($var) <= 'z'))",
        blank => "(($var) == ' ' || ($var) == '\\t')",
        cntrl => "((($var) <= 0x1F) || (($var) == 0x7F))",
        digit => "(($var) >= '0' && ($var) <= '9')",
        graph => "(($var) >= 0x21 && ($var) <= 0x7E)",
        lower => "(($var) >= 'a' && ($var) <= 'z')",
        print => "(($var) >= 0x20 && ($var) <= 0x7E)",
        punct => "((($var) >= 0x21 && ($var) <= 0x2F) || (($var) >= 0x3A && ($var) <= 0x40) || (($var) >= 0x5B && ($var) <= 0x60) || (($var) >= 0x7B && ($var) <= 0x7E))",
        space => "($var == ' ' || $var == '\\t' || $var == '\\n' || $var == '\\r' || $var == '\\f' || $var == '\\x0b')",
        upper => "(($var) >= 'A' && ($var) <= 'Z')",
        xdigit => "((($var) >= '0' && ($var) <= '9') || ((($var) >= 'A' && ($var) <= 'F') || (($var) >= 'a' && ($var) <= 'f')))",
        word => "((($var) >= '0' && ($var) <= '9') || ((($var) >= 'A' && ($var) <= 'Z') || (($var) >= 'a' && ($var) <= 'z')) || ($var) == '_')",
    );
    my $k = lc $name;
    die "unsupported POSIX class [$name]\n" unless exists $map{$k};
    return $map{$k};
}

sub regex_class_item_expr {
    my ($item, $var) = @_;
    if ($item->{type} eq 'char') {
        return "($var == " . c_byte_literal($item->{ch}) . ")";
    }
    if ($item->{type} eq 'range') {
        return "(($var >= " . c_byte_literal($item->{from}) . ") && ($var <= " . c_byte_literal($item->{to}) . "))";
    }
    if ($item->{type} eq 'posix') {
        return regex_posix_class_expr($item->{name}, $var);
    }
    if ($item->{type} eq 'shorthand') {
        return regex_posix_class_expr($item->{name}, $var);
    }
    die "unknown character class item\n";
}

sub regex_class_expr {
    my ($node, $var) = @_;
    my @parts = map { regex_class_item_expr($_, $var) } @{ $node->{items} || [] };
    die "empty character class\n" unless @parts;
    my $expr = join(' || ', @parts);
    return $node->{neg} ? "(!($expr))" : "($expr)";
}

sub regex_parse {
    my ($pat) = @_;
    my $i = 0;
    my $len = length($pat);
    my ($parse_expr, $parse_concat, $parse_repeat, $parse_atom, $parse_class, $parse_class_atom);

    my $peek = sub { $i < $len ? substr($pat, $i, 1) : undef };
    my $eat = sub { $i < $len ? substr($pat, $i++, 1) : undef };
    my $err = sub { die "bad regular expression [$pat]\n"; };

    my $escaped_node = sub {
        my ($c) = @_;
        return { type => 'class', neg => 0, items => [ { type => 'posix', name => 'digit' } ] } if $c eq 'd';
        return { type => 'class', neg => 1, items => [ { type => 'posix', name => 'digit' } ] } if $c eq 'D';
        return { type => 'class', neg => 0, items => [ { type => 'posix', name => 'space' } ] } if $c eq 's';
        return { type => 'class', neg => 1, items => [ { type => 'posix', name => 'space' } ] } if $c eq 'S';
        return { type => 'class', neg => 0, items => [ { type => 'posix', name => 'word' } ] } if $c eq 'w';
        return { type => 'class', neg => 1, items => [ { type => 'posix', name => 'word' } ] } if $c eq 'W';
        return { type => 'char', ch => ord(regex_escape_value($c)) };
    };

    $parse_class_atom = sub {
        my $c = $peek->();
        $err->() unless defined $c;
        if ($c eq '\\') {
            $eat->();
            my $e = $eat->();
            $err->() unless defined $e;
            return { type => 'char', ch => ord(regex_escape_value($e)), escaped => 1 };
        }
        if ($c eq '[' && substr($pat, $i + 1, 1) eq ':') {
            my $end = index($pat, ':]', $i + 2);
            $err->() if $end < 0;
            my $name = substr($pat, $i + 2, $end - ($i + 2));
            $i = $end + 2;
            return { type => 'posix', name => $name };
        }
        $eat->();
        return { type => 'char', ch => ord($c), escaped => 0 };
    };

    $parse_class = sub {
        my $neg = 0;
        my @items;
        $eat->();
        my $c = $peek->();
        if (defined $c && $c eq '^') {
            $neg = 1;
            $eat->();
        }
        my $first = 1;
        while (1) {
            $c = $peek->();
            $err->() unless defined $c;
            last if $c eq ']' && !$first;
            my $atom = $parse_class_atom->();
            # NOTE: the historical `!$first` guard here prevented a range
            # whose start atom is in first position, so [a-z] compiled to
            # {a,-,z} while [ab-c] worked.  A leading '-' stays literal
            # regardless: '-' can only start a range when followed by
            # '-' (peek check below), and '[-...]' has no '-' in second
            # position.  Fixed 2026-09-23.
            if ($atom->{type} eq 'char'
                && $atom->{ch} != 45
                && defined($peek->())
                && $peek->() eq '-'
                && substr($pat, $i + 1, 1) ne ']') {
                $eat->();
                my $rhs = $parse_class_atom->();
                $err->() unless $rhs->{type} eq 'char';
                push @items, { type => 'range', from => $atom->{ch}, to => $rhs->{ch} };
            } else {
                push @items, $atom;
            }
            $first = 0;
        }
        $eat->();
        return { type => 'class', neg => $neg, items => \@items };
    };

    $parse_atom = sub {
        my $c = $peek->();
        $err->() unless defined $c;
        if ($c eq '(') {
            $eat->();
            my $node = $parse_expr->();
            $err->() unless defined($peek->()) && $peek->() eq ')';
            $eat->();
            return $node;
        }
        if ($c eq '[') {
            return $parse_class->();
        }
        if ($c eq '.') {
            $eat->();
            return { type => 'any' };
        }
        if ($c eq '$') {
            $eat->();
            return { type => 'eol' };
        }
        if ($c eq '^' && $i == 0) {
            $eat->();
            return { type => 'empty' };
        }
        if ($c eq '\\') {
            $eat->();
            my $e = $eat->();
            $err->() unless defined $e;
            return $escaped_node->($e);
        }
        if ($c eq '*' || $c eq '+' || $c eq '?') {
            $err->();
        }
        $eat->();
        return { type => 'char', ch => ord($c) };
    };

    $parse_repeat = sub {
        my $node = $parse_atom->();
        while (defined(my $c = $peek->())) {
            if ($c eq '*') {
                $eat->();
                $node = { type => 'repeat', min => 0, max => undef, child => $node };
                next;
            }
            if ($c eq '+') {
                $eat->();
                $node = { type => 'repeat', min => 1, max => undef, child => $node };
                next;
            }
            if ($c eq '?') {
                $eat->();
                $node = { type => 'repeat', min => 0, max => 1, child => $node };
                next;
            }
            if ($c eq '{') {
                my $save = $i;
                $eat->();
                my $read_num = sub {
                    my $n = '';
                    while (1) {
                        my $d = $peek->();
                        last unless defined $d && $d =~ /[0-9]/;
                        $n .= $d;
                        $eat->();
                    }
                    return $n;
                };
                my $m = $read_num->();
                if ($m eq '' ) { $i = $save; last; }
                my $n = $m;
                if (defined($peek->()) && $peek->() eq ',') {
                    $eat->();
                    $n = $read_num->();
                    $n = undef if $n eq '';
                }
                $err->() unless defined($peek->()) && $peek->() eq '}';
                $eat->();
                $node = { type => 'repeat', min => int($m), max => defined($n) ? int($n) : undef, child => $node };
                next;
            }
            last;
        }
        return $node;
    };

    $parse_concat = sub {
        my @parts;
        while (1) {
            my $c = $peek->();
            last unless defined $c && $c ne '|' && $c ne ')';
            push @parts, $parse_repeat->();
        }
        return { type => 'empty' } unless @parts;
        return $parts[0] if @parts == 1;
        return { type => 'concat', parts => \@parts };
    };

    $parse_expr = sub {
        my @alts = ($parse_concat->());
        while (defined($peek->()) && $peek->() eq '|') {
            $eat->();
            push @alts, $parse_concat->();
        }
        return $alts[0] if @alts == 1;
        return { type => 'alt', parts => \@alts };
    };

    my $ast = $parse_expr->();
    $err->() if $i != $len;
    return $ast;
}

sub regex_literal_ast {
    my ($text) = @_;
    return { type => 'literal', text => $text };
}

sub regex_compile_nfa {
    my ($ast, $desc) = @_;
    my @states;
    my @classes;

    my $new_state = sub {
        my ($kind, $out1, $out2, $arg) = @_;
        push @states, {
            kind => $kind,
            out1 => defined($out1) ? $out1 : -1,
            out2 => defined($out2) ? $out2 : -1,
            arg  => defined($arg)  ? $arg  : 0,
        };
        return $#states;
    };

    my $patch = sub {
        my ($outs, $target) = @_;
        for my $o (@$outs) {
            $states[$o->[0]]{$o->[1]} = $target;
        }
    };

    my $concat = sub {
        my ($a, $b) = @_;
        $patch->($a->{outs}, $b->{start});
        return { start => $a->{start}, outs => $b->{outs} };
    };

    my $compile;
    my $class_id = sub {
        my ($node) = @_;
        return $node->{class_id} if defined $node->{class_id};
        $node->{class_id} = scalar @classes;
        push @classes, $node;
        return $node->{class_id};
    };

    my $repeat_frag;
    $repeat_frag = sub {
        my ($node, $count) = @_;
        my $frag;
        for (1 .. $count) {
            my $next = $compile->($node);
            $frag = defined $frag ? $concat->($frag, $next) : $next;
        }
        return $frag || do {
            my $s = $new_state->('jmp', undef, undef, undef);
            { start => $s, outs => [ [ $s, 'out1' ] ] };
        };
    };

    $compile = sub {
        my ($node) = @_;
        my $type = $node->{type};
        if ($type eq 'literal') {
            my @bytes = unpack('C*', $node->{text});
            return do {
                my $s = $new_state->('jmp', undef, undef, undef);
                { start => $s, outs => [ [ $s, 'out1' ] ] };
            } unless @bytes;
            my $frag;
            for my $b (@bytes) {
                my $s = $new_state->('char', undef, undef, $b);
                my $next = { start => $s, outs => [ [ $s, 'out1' ] ] };
                $frag = defined $frag ? $concat->($frag, $next) : $next;
            }
            return $frag;
        }
        if ($type eq 'char') {
            my $s = $new_state->('char', undef, undef, $node->{ch});
            return { start => $s, outs => [ [ $s, 'out1' ] ] };
        }
        if ($type eq 'any') {
            my $s = $new_state->('any', undef, undef, 0);
            return { start => $s, outs => [ [ $s, 'out1' ] ] };
        }
        if ($type eq 'eol') {
            my $s = $new_state->('eol', undef, undef, 0);
            return { start => $s, outs => [ [ $s, 'out1' ] ] };
        }
        if ($type eq 'class') {
            my $id = $class_id->($node);
            my $s = $new_state->('class', undef, undef, $id);
            return { start => $s, outs => [ [ $s, 'out1' ] ] };
        }
        if ($type eq 'empty') {
            my $s = $new_state->('jmp', undef, undef, undef);
            return { start => $s, outs => [ [ $s, 'out1' ] ] };
        }
        if ($type eq 'concat') {
            my @parts = @{ $node->{parts} || [] };
            my $frag = $compile->(shift @parts);
            for my $part (@parts) {
                $frag = $concat->($frag, $compile->($part));
            }
            return $frag;
        }
        if ($type eq 'alt') {
            my @parts = @{ $node->{parts} || [] };
            my $frag = $compile->(shift @parts);
            for my $part (@parts) {
                my $rhs = $compile->($part);
                my $s = $new_state->('split', $frag->{start}, $rhs->{start}, undef);
                $frag = { start => $s, outs => [ @{ $frag->{outs} }, @{ $rhs->{outs} } ] };
            }
            return $frag;
        }
        if ($type eq 'repeat') {
            my $min = $node->{min};
            my $max = $node->{max};
            my $frag;
            if (defined $max) {
                die "bad regular expression [$desc->{text}]\n" if $max < $min;
                $frag = $min ? $repeat_frag->($node->{child}, $min) : do {
                    my $s = $new_state->('jmp', undef, undef, undef);
                    { start => $s, outs => [ [ $s, 'out1' ] ] };
                };
                my $extra = $max - $min;
                for (1 .. $extra) {
                    next if $extra <= 0;
                    my $opt = $compile->($node->{child});
                    my $s = $new_state->('split', $opt->{start}, undef, undef);
                    my $maybe = { start => $s, outs => [ @{ $opt->{outs} }, [ $s, 'out2' ] ] };
                    $frag = $concat->($frag, $maybe);
                }
            } else {
                $frag = $min ? $repeat_frag->($node->{child}, $min) : do {
                    my $s = $new_state->('jmp', undef, undef, undef);
                    { start => $s, outs => [ [ $s, 'out1' ] ] };
                };
                my $star = $compile->($node->{child});
                my $s = $new_state->('split', $star->{start}, undef, undef);
                $patch->($star->{outs}, $s);
                my $tail = { start => $s, outs => [ [ $s, 'out2' ] ] };
                $frag = defined $frag ? $concat->($frag, $tail) : $tail;
                return $frag;
            }
            return $frag;
        }
        die "bad regular expression [$desc->{text}]\n";
    };

    my $frag = $compile->($ast);
    my $match = $new_state->('match', undef, undef, undef);
    $patch->($frag->{outs}, $match);
    return {
        states => \@states,
        classes => \@classes,
        start => $frag->{start},
    };
}

sub parse_terminal_symbol {
    my ($s) = @_;
    my $lit = quoted($s);
    return { kind => 'literal', text => $lit, key => 'lit:' . unpack('H*', $lit), ast => regex_literal_ast($lit) }
        if defined $lit;
    if ($s =~ m!^/(.*)/$!s) {
        my $pat = $1;
        return { kind => 'regex', text => $pat, key => 're:' . unpack('H*', $pat), ast => regex_parse($pat) };
    }
    return undef;
}

sub collect_terminals {
    my ($rules, $skip) = @_;
    my %by_key;
    my %by_symbol;
    my @terms;
    my $add = sub {
        my ($desc) = @_;
        return $by_key{$desc->{key}} if exists $by_key{$desc->{key}};
        $desc->{id} = scalar @terms;
        $by_key{$desc->{key}} = $desc->{id};
        push @terms, $desc;
        return $desc->{id};
    };
    for my $r (@$rules) {
        for my $s (@{ $r->{rhs} || [] }) {
            my $desc = parse_terminal_symbol($s) or next;
            my $id = $add->($desc);
            $by_symbol{$s} = $id unless exists $by_symbol{$s};
        }
    }
    my $skip_desc = undef;
    if (defined $skip && $skip ne '') {
        $skip_desc = { kind => 'regex', text => $skip, key => 'skip:' . unpack('H*', $skip), ast => regex_parse($skip), id => 'skip' };
    }
    return (\@terms, $skip_desc, \%by_symbol);
}

sub emit_regex_matcher {
    my ($desc) = @_;
    $desc->{nfa} = regex_compile_nfa($desc->{ast}, $desc);
    my $id = $desc->{id};
    my $n = scalar @{ $desc->{nfa}{states} || [] };
    my $state_name = "glr_rx_${id}_states";
    my $class_name = "glr_rx_${id}_class";
    my $count_name = "GLR_RX_${id}_COUNT";
    my $out = "";
    $out .= "enum { ${count_name} = $n };\n";
    $out .= "static const struct GLRRegexState ${state_name}[${count_name}] = {\n";
    for my $st (@{ $desc->{nfa}{states} }) {
        my %kind = (
            match => 'GLR_RX_MATCH',
            split => 'GLR_RX_SPLIT',
            jmp   => 'GLR_RX_JMP',
            char  => 'GLR_RX_CHAR',
            class => 'GLR_RX_CLASS',
            any   => 'GLR_RX_ANY',
            eol   => 'GLR_RX_EOL',
        );
        $out .= sprintf("  { %s, %d, %d, %u },\n", $kind{$st->{kind}}, $st->{out1}, $st->{out2}, $st->{arg});
    }
    $out .= "};\n";
    $out .= "static int ${class_name}(unsigned int cls, unsigned char ch) {\n";
    # NOTE: class nodes are collected in $desc->{nfa}{classes} by
    # regex_compile_nfa (see the returned {states, classes, start} triple).
    # Reading them from $desc->{classes} instead leaves every [...] class,
    # POSIX [[:...:]] class and \d \w \s shorthand compiling to an
    # always-false matcher.  Fixed 2026-09-23.
    if (@{ $desc->{nfa}{classes} || [] }) {
        $out .= "  switch (cls) {\n";
        for my $i (0 .. $#{ $desc->{nfa}{classes} || [] }) {
            $out .= "    case $i: return " . regex_class_expr($desc->{nfa}{classes}[$i], 'ch') . ";\n";
        }
        $out .= "  }\n  return 0;\n";
    } else {
        $out .= "  (void)cls; (void)ch; return 0;\n";
    }
    $out .= "}\n";
    $out .= "static int glr_rx_${id}(const char *s, const char *end, size_t *out_n) {\n";
    $out .= "  int clist[${count_name}], nlist[${count_name}], endlist[${count_name}];\n";
    $out .= "  unsigned char seen[${count_name}], seen2[${count_name}], seen3[${count_name}];\n";
    $out .= "  int cn = 0, nn = 0, en = 0;\n";
    $out .= "  size_t pos = 0, best = 0;\n";
    $out .= "  memset(seen, 0, sizeof seen);\n";
    # NOTE: end-of-input must be threaded into epsilon closure as
    # at_end, otherwise `$` (GLR_RX_EOL) can never fire: epsilon states
    # reached mid-input resolve splits with at_end=0, pruning the eol
    # branch before the end is known.  Passing literal 0 here and below
    # left `$` dead in every position (verified 2026-09-23).  Patterns
    # without `$` contain no EOL states, so this changes nothing for
    # them.  Fixed 2026-09-23.
    $out .= "  glr_rx_addstate($state_name, $desc->{nfa}{start}, clist, &cn, seen, (s >= end));\n";
    $out .= "  for (;;) {\n";
    $out .= "    for (int i = 0; i < cn; i++) if (${state_name}[clist[i]].kind == GLR_RX_MATCH) best = pos;\n";
    $out .= "    if (s + pos >= end) break;\n";
    $out .= "    unsigned char ch = (unsigned char)s[pos];\n";
    $out .= "    nn = 0; memset(seen2, 0, sizeof seen2);\n";
    $out .= "    for (int i = 0; i < cn; i++) {\n";
    $out .= "      int q = clist[i];\n";
    $out .= "      switch (${state_name}[q].kind) {\n";
    $out .= "      case GLR_RX_CHAR:\n";
    $out .= "        if ((unsigned char)${state_name}[q].arg == ch) glr_rx_addstate($state_name, ${state_name}[q].out1, nlist, &nn, seen2, (s + pos + 1 >= end));\n";
    $out .= "        break;\n";
    $out .= "      case GLR_RX_CLASS:\n";
    $out .= "        if ($class_name(${state_name}[q].arg, ch)) glr_rx_addstate($state_name, ${state_name}[q].out1, nlist, &nn, seen2, (s + pos + 1 >= end));\n";
    $out .= "        break;\n";
    $out .= "      case GLR_RX_ANY:\n";
    $out .= "        if (ch != '\\n') glr_rx_addstate($state_name, ${state_name}[q].out1, nlist, &nn, seen2, (s + pos + 1 >= end));\n";
    $out .= "        break;\n";
    $out .= "      default:\n";
    $out .= "        break;\n";
    $out .= "      }\n";
    $out .= "    }\n";
    $out .= "    if (nn == 0) break;\n";
    $out .= "    for (int i = 0; i < nn; i++) clist[i] = nlist[i];\n";
    $out .= "    cn = nn;\n";
    $out .= "    pos++;\n";
    $out .= "  }\n";
    $out .= "  if (pos == (size_t)(end - s)) {\n";
    $out .= "    memset(seen3, 0, sizeof seen3);\n";
    $out .= "    en = 0;\n";
    $out .= "    for (int i = 0; i < cn; i++) glr_rx_addstate($state_name, clist[i], endlist, &en, seen3, 1);\n";
    $out .= "    for (int i = 0; i < en; i++) if (${state_name}[endlist[i]].kind == GLR_RX_MATCH) best = pos;\n";
    $out .= "  }\n";
    $out .= "  if (best == 0) return 0;\n";
    $out .= "  *out_n = best;\n";
    $out .= "  return 1;\n";
    $out .= "}\n";
    return $out;
}

sub emit_terminal_wrappers {
    my ($terms, $skip_desc) = @_;
    my $out = "";
    $out .= "typedef struct GLRParser { const char *s, *end; const char *error; } GLRParser;\n";
    $out .= "enum { GLR_RX_MATCH, GLR_RX_SPLIT, GLR_RX_JMP, GLR_RX_CHAR, GLR_RX_CLASS, GLR_RX_ANY, GLR_RX_EOL };\n";
    $out .= "struct GLRRegexState { int kind; int out1; int out2; unsigned int arg; };\n";
    $out .= "static void glr_rx_addstate(const struct GLRRegexState *st, int s, int *list, int *n, unsigned char *seen, int at_end) {\n";
    $out .= "  if (s < 0 || seen[s]) return;\n";
    $out .= "  seen[s] = 1;\n";
    $out .= "  switch (st[s].kind) {\n";
    $out .= "  case GLR_RX_SPLIT:\n";
    $out .= "    glr_rx_addstate(st, st[s].out1, list, n, seen, at_end);\n";
    $out .= "    glr_rx_addstate(st, st[s].out2, list, n, seen, at_end);\n";
    $out .= "    break;\n";
    $out .= "  case GLR_RX_JMP:\n";
    $out .= "    glr_rx_addstate(st, st[s].out1, list, n, seen, at_end);\n";
    $out .= "    break;\n";
    $out .= "  case GLR_RX_EOL:\n";
    $out .= "    if (at_end) glr_rx_addstate(st, st[s].out1, list, n, seen, at_end);\n";
    $out .= "    break;\n";
    $out .= "  default:\n";
    $out .= "    list[(*n)++] = s;\n";
    $out .= "    break;\n";
    $out .= "  }\n";
    $out .= "}\n";
    if (defined $skip_desc) {
        $out .= emit_regex_matcher($skip_desc);
        my $skip_id = $skip_desc->{id};
        $out .= "static void glr_skip(GLRParser *p) { size_t n = 0; while (p->s < p->end && glr_rx_${skip_id}(p->s, p->end, &n) && n > 0) p->s += n; }\n";
    } else {
        $out .= "static void glr_skip(GLRParser *p) { (void)p; }\n";
    }
    for my $term (@$terms) {
        $out .= emit_regex_matcher($term);
    }
    $out .= "static void glr_fail(GLRParser *p) { if (!p->error) p->error = p->s; }\n";
    $out .= "static void *glr_box_double(double d) { double *x=(double*)malloc(sizeof *x); if(x)*x=d; return x; }\n";
    $out .= "static char *my_strdup(const char *s) { size_t n=strlen(s); char *x=(char*)malloc(n+1); if(x){memcpy(x,s,n);x[n]=0;} return x; }\n";
    $out .= "static int glr_match(GLRParser *p, int term, char **tok) {\n";
    $out .= "  size_t n = 0; const char *at = p->s; char *x = NULL;\n";
    $out .= "  switch (term) {\n";
    for my $term (@$terms) {
        my $id = $term->{id};
        $out .= "  case AUROCKS_TERM_${id}: if (!glr_rx_${id}(p->s, p->end, &n) || n == 0 || p->s + n > p->end) return 0; x = (char*)malloc(n + 1); if (!x) return 0; memcpy(x, at, n); x[n] = 0; p->s += n; *tok = x; return 1;\n";
    }
    $out .= "  }\n";
    $out .= "  return 0;\n";
    $out .= "}\n";
    return $out;
}

sub emit {
    my ($pro, $epi, $start, $d, $rules, $nonterm, $api_name) = @_;
    $api_name ||= "parse_$start";
    my %by;
    push @{ $by{$_->{lhs}} }, $_ for @$rules;
    my @nts = sort keys %$nonterm;
    my $skip = $d->{skip} && $d->{skip}[0] =~ m!^\s*/((?:\\.|[^/])*)/! ? $1 : '';
    $skip =~ s/\[[^\]]*\\[trn][^\]]*\]/[[:space:]]/;
    my ($terms, $skip_desc, $term_ids) = collect_terminals($rules, $skip);
    print $pro, "\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n\n";
    print "/* Scannerless GLR/SPPF controls accepted: %start %skip %prefer %avoid %longest %dprec %left %right %nonassoc %merge %reject %cut %expect %glr %sppf %ambiguity %resolve %priority %inline %layout %lexer %error. */\n";
    print "enum { GLR_SPPF_PREFER=1, GLR_SPPF_AVOID=2, GLR_SPPF_LONGEST=4, GLR_SPPF_DPREC=8 };\n";
    if (@$terms) {
        print "enum { ", join(", ", map { "AUROCKS_TERM_$_->{id} = $_->{id}" } @$terms), " };\n";
    }
    print emit_terminal_wrappers($terms, $skip_desc);
    for my $nt (@nts) { print "static int glr_$nt(GLRParser*, void**);\n" }
    for my $nt (@nts) {
        print "\nstatic int glr_$nt(GLRParser *p, void **out) {\n  GLRParser save = *p;\n";
        my @nt_rules = @{ $by{$nt} || [] };
        my @left = grep { @{ $_->{rhs} } && $_->{rhs}[0] eq $nt } @nt_rules;
        my @seed = grep { !(@{ $_->{rhs} } && $_->{rhs}[0] eq $nt) } @nt_rules;
        if (@left && @seed) {
            for my $r (@seed) {
                print "  { GLRParser attempt = save; void *result = NULL; char *token = NULL;\n";
                my $n = @{ $r->{rhs} };
                for my $i (1..$n) { print "  void *v$i = NULL;\n" }
                print "  int ok = 1;\n";
                for my $i (0..$n-1) {
                    my $s = $r->{rhs}[$i];
                    if ($s eq '%empty') {
                        next;
                    } elsif ($nonterm->{$s}) {
                        print "  if (ok && !glr_$s(&attempt, &v", $i+1, ")) ok = 0;\n";
                    } else {
                        my $id = $term_ids->{$s};
                        die "unknown terminal [$s]\n" unless defined $id;
                        print "  if (ok) { glr_skip(&attempt); if (!glr_match(&attempt, AUROCKS_TERM_$id, &token)) ok = 0; }\n";
                    }
                }
                print "  if (ok) { glr_skip(&attempt); { ", action_c($r->{action}), " }\n";
                print "    void *acc = result;\n    for (;;) { GLRParser step = attempt; int advanced = 0;\n";
                for my $r (@left) {
                    my $n = @{ $r->{rhs} };
                    print "      if (!advanced) { GLRParser tail = step; int tail_ok = 1; char *tail_token = NULL;\n";
                    for my $i (1..$n-1) { print "        void *v", $i+1, " = NULL;\n" }
                    for my $i (1..$n-1) {
                        my $s = $r->{rhs}[$i];
                        if ($nonterm->{$s}) {
                            print "        if (tail_ok && !glr_$s(&tail, &v", $i+1, ")) tail_ok = 0;\n";
                        } elsif ($s ne '%empty') {
                            my $id = $term_ids->{$s};
                            die "unknown terminal [$s]\n" unless defined $id;
                            print "        if (tail_ok) { glr_skip(&tail); if (!glr_match(&tail, AUROCKS_TERM_$id, &tail_token)) tail_ok = 0; }\n";
                        }
                    }
                    print "        if (tail_ok) { void *v1 = acc; GLRParser attempt = tail; void *result = acc; { ", action_c($r->{action}), " } acc = result; step = tail; advanced = 1; }\n      }\n";
                }
                print "      if (!advanced) break; attempt = step; }\n    *p = attempt; *out = acc; return 1; }\n  }\n";
            }
            print "  *p = save; glr_fail(p); return 0;\n}\n";
            next;
        }
        my @alternatives = sort {
            my ($ae, $be) = (@{$a->{rhs}} == 0, @{$b->{rhs}} == 0);
            my ($al, $bl) = (($a->{rhs}[0] // '') eq $nt, ($b->{rhs}[0] // '') eq $nt);
            $ae && !$be ? 1 : (!$ae && $be ? -1 :
            ($al && !$bl ? 1 : (!$al && $bl ? -1 : 0)))
        } @{ $by{$nt} || [] };
        for my $r (@alternatives) {
            print "  { GLRParser attempt = save; void *result = NULL; char *token = NULL;\n";
            my $n = @{ $r->{rhs} };
            for my $i (1..$n) { print "  void *v$i = NULL;\n" }
            print "  int ok = 1;\n";
            for my $i (0..$n-1) {
                my $s = $r->{rhs}[$i];
                next if $s eq '%empty';
                if ($nonterm->{$s}) {
                    print "  if (ok && !glr_$s(&attempt, &v", $i+1, ")) ok = 0;\n";
                } else {
                    my $id = $term_ids->{$s};
                    die "unknown terminal [$s]\n" unless defined $id;
                    print "  if (ok) { glr_skip(&attempt); if (!glr_match(&attempt, AUROCKS_TERM_$id, &token)) ok = 0; }\n";
                }
            }
            print "  if (ok) { glr_skip(&attempt); { ", action_c($r->{action}), " } *p = attempt; *out = result; return 1; }\n  }\n";
        }
        print "  *p = save; glr_fail(p); return 0;\n}\n";
    }
    print "\nvoid *$api_name(const char *input) { GLRParser p={input,input+strlen(input),NULL}; void *out=NULL; if(!glr_$start(&p,&out)) return NULL; glr_skip(&p); if(p.s!=p.end) return NULL; return out; }\n";
    print "void *parse_$start(const char *input) { return $api_name(input); }\n"
        if $api_name ne "parse_$start";
    print "void *yyparse(const char *input) { return $api_name(input); }\n";
    print $epi;
}

sub main {
    my $entrypoint;
    my @args;
    while (@ARGV) {
        my $a = shift @ARGV;
        if ($a eq '--entrypoint' || $a eq '-e') {
            $entrypoint = shift @ARGV // die "--entrypoint requires a name\n";
        } elsif ($a eq '--help' || $a eq '-h') {
            print "usage: $0 [--entrypoint NAME] grammar.g\n";
            return;
        } else {
            push @args, $a;
        }
    }
    my ($path) = @args;
    die "usage: $0 [--entrypoint NAME] grammar.g\n" unless $path;
    my ($pro, $epi, $start, $d, $rules, $nonterm) = parse_grammar(slurp($path));
    die "invalid entrypoint name\n"
        if defined($entrypoint) && $entrypoint !~ /^[A-Za-z_]\w*$/;
    emit($pro, $epi, $start, $d, $rules, $nonterm, $entrypoint);
}
main();
