package Xapian::MultiValueSorter;

=head1 NAME

Xapian::MultiValueSorter - allows sorting by a several values.

=head1 DESCRIPTION

Results are ordered by the first value.  In the event of a tie, the
second is used.  If this is the same for both, the third is used, and
so on.

=head1 SYNOPSIS

  use Xapian;

  my $db = new Xapian::Database("/path/to/db");
  my $enq = new Xapian::Enquire($db);
  my $sorter = new Xapian::MultiValueSorter(1, 3, 5);
  $enq->set_sort_by_key($sorter);

=head1 METHODS

=over 4

=item new [<value>...]

Construct and add the values listed.

=item add <value> [<forward>]

Add another value to sort on.  By default, values sort forwards, unless forward
is specified and is false.

=back

=head1 REFERENCE

  http://www.xapian.org/docs/sourcedoc/html/classXapian_1_1MultiValueSorter.html

=cut
1;
