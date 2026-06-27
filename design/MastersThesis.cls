% MastersThesis.cls — Timeline Compiler Technical Specification Class
% 
% This is a self-contained document class providing professional defaults
% for the Timeline Compiler specification document. It can be swapped for
% the user's preferred MastersThesis.cls later if desired.
%
% Requirements: Standard TeX Live install with geometry, fancyhdr, titlesec,
% enumitem, hyperref, xcolor packages (all standard in TeX Live).

\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{MastersThesis}[2026/06/09 Timeline Compiler Spec Class]

% Base class
\LoadClass[11pt,a4paper,oneside]{report}

% ============================================================================
% PACKAGES
% ============================================================================

% Page geometry — generous margins for readability
\RequirePackage[
  top=1in,
  bottom=1in,
  left=1.25in,
  right=1.25in,
  headheight=14pt
]{geometry}

% Typography
\RequirePackage[T1]{fontenc}
\RequirePackage{lmodern}          % Latin Modern fonts
\RequirePackage{microtype}        % Microtypographic improvements

% Headers and footers
\RequirePackage{fancyhdr}

% Section styling
\RequirePackage{titlesec}

% Lists
\RequirePackage{enumitem}

% Colors
\RequirePackage[dvipsnames,svgnames,x11names]{xcolor}

% Hyperlinks (load last among these)
\RequirePackage[
  colorlinks=true,
  linkcolor=Navy,
  citecolor=DarkGreen,
  urlcolor=DarkBlue,
  bookmarksnumbered=true,
  bookmarksopen=true
]{hyperref}

% ============================================================================
% COLOR DEFINITIONS
% ============================================================================

\definecolor{ChapterColor}{RGB}{0,51,102}
\definecolor{SectionColor}{RGB}{0,76,153}
\definecolor{SubsectionColor}{RGB}{51,102,153}

% ============================================================================
% SECTION STYLING
% ============================================================================

\titleformat{\chapter}[display]
  {\normalfont\huge\bfseries\color{ChapterColor}}
  {\chaptertitlename\ \thechapter}
  {20pt}
  {\Huge}
  
\titleformat{\section}
  {\normalfont\Large\bfseries\color{SectionColor}}
  {\thesection}
  {1em}
  {}

\titleformat{\subsection}
  {\normalfont\large\bfseries\color{SubsectionColor}}
  {\thesubsection}
  {1em}
  {}

\titleformat{\subsubsection}
  {\normalfont\normalsize\bfseries}
  {\thesubsubsection}
  {1em}
  {}

% Spacing around sections
\titlespacing*{\chapter}{0pt}{0pt}{30pt}
\titlespacing*{\section}{0pt}{24pt plus 4pt minus 2pt}{12pt plus 2pt minus 2pt}
\titlespacing*{\subsection}{0pt}{18pt plus 3pt minus 2pt}{8pt plus 2pt minus 1pt}
\titlespacing*{\subsubsection}{0pt}{12pt plus 2pt minus 1pt}{6pt plus 1pt minus 1pt}

% ============================================================================
% HEADERS AND FOOTERS
% ============================================================================

\pagestyle{fancy}
\fancyhf{}
\fancyhead[L]{\nouppercase{\leftmark}}
\fancyhead[R]{\thepage}
\renewcommand{\headrulewidth}{0.4pt}
\renewcommand{\footrulewidth}{0pt}

% Plain style for chapter pages
\fancypagestyle{plain}{
  \fancyhf{}
  \fancyfoot[C]{\thepage}
  \renewcommand{\headrulewidth}{0pt}
}

% ============================================================================
% LIST STYLING
% ============================================================================

\setlist[itemize]{
  topsep=6pt,
  itemsep=3pt,
  parsep=0pt,
  leftmargin=2em
}

\setlist[enumerate]{
  topsep=6pt,
  itemsep=3pt,
  parsep=0pt,
  leftmargin=2em
}

\setlist[description]{
  topsep=6pt,
  itemsep=6pt,
  parsep=0pt,
  leftmargin=0pt,
  style=nextline
}

% ============================================================================
% TITLE PAGE
% ============================================================================

\renewcommand{\maketitle}{%
  \begin{titlepage}
    \centering
    \vspace*{2cm}
    
    {\Huge\bfseries\color{ChapterColor} \@title \par}
    
    \vspace{1.5cm}
    
    \rule{\textwidth}{1pt}
    
    \vspace{2cm}
    
    {\Large \@author \par}
    
    \vfill
    
    {\large \@date \par}
    
    \vspace{2cm}
  \end{titlepage}
  \setcounter{page}{1}
}

% ============================================================================
% PARAGRAPH STYLING
% ============================================================================

\setlength{\parindent}{0pt}
\setlength{\parskip}{10pt plus 2pt minus 1pt}

% ============================================================================
% TABLE OF CONTENTS DEPTH
% ============================================================================

\setcounter{tocdepth}{3}
\setcounter{secnumdepth}{3}

\endinput
